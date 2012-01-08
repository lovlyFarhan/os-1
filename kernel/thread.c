#include <stdlib.h>

#include <sys/arch.h>
#include <sys/spinlock.h>

#include <kernel/assert.h>
#include <kernel/thread.h>
#include <kernel/vm.h>

static LIST_HEAD(normal_ready_queue);
static LIST_HEAD(io_ready_queue);
static Spinlock_t ready_queue_lock = SPINLOCK_INIT;

static inline struct list_head * queue_for_thread (struct Thread * t)
{
    if (t->assigned_priority == THREAD_PRIORITY_IO || t->effective_priority == THREAD_PRIORITY_IO) {
        return &io_ready_queue;
    }
    else {
        return &normal_ready_queue;
    }
}

static void thread_entry (ThreadFunc func, void * param);

void ThreadYieldNoRequeueToSpecific (struct Thread * next);

typedef void (*ThreadSwitchPreFunc) (void *param);

static void ThreadSwitch (
        struct Thread * outgoing,
        struct Thread * incoming,
        ThreadSwitchPreFunc func,
        void * funcParam
        )
{
    uint32_t next_pc = next_pc;         /* Used by assembly fragment    */
    uint32_t cpsr_temp = cpsr_temp;     /* Used by assembly fragment    */

    struct TranslationTable * incoming_tt = incoming->process
            ? incoming->process->pagetable
            : NULL;

    /* Turn off interrupts */
    IrqSave_t prev_irq_status = InterruptsDisable();
    assert(prev_irq_status.cpsr_interrupt_flags == 0);

    if (func != NULL) {
        func(funcParam);
    }

    /* Stash prior interrupt state in incoming thread's saved CPSR */
    incoming->registers[REGISTER_INDEX_PSR] &= ~(SETBIT(ARM_CPSR_I_BIT) | SETBIT(ARM_CPSR_F_BIT));
    incoming->registers[REGISTER_INDEX_PSR] |= prev_irq_status.cpsr_interrupt_flags;

    /* Only flushes TLB if the new data structure isn't the same as the old one */
    MmuSetUserTranslationTable(incoming_tt);

    /* Mark incoming thread as running */
    incoming->state = THREAD_STATE_RUNNING;

    asm volatile(
        "                                                   \n\t"
        "save_outgoing$:                                    \n\t"
        "    /* Store normal registers */                   \n\t"
        "    stm %[p_outgoing], {r0 - r15}                  \n\t"
        "                                                   \n\t"
        "    /* Store CPSR modulo the IRQ mask */           \n\t"
        "    mrs %[cpsr_temp], cpsr                         \n\t"
        "    bic %[cpsr_temp], %[cpsr_temp], %[int_bits]    \n\t"
        "    orr %[cpsr_temp], %[cpsr_temp], %[prev_irq]    \n\t"
        "    str %[cpsr_temp], [%[p_outgoing_cpsr], #0]     \n\t"
        "                                                   \n\t"
        "    /* Patch up stored PC to be at resume point */ \n\t"
        "    ldr %[next_pc], =resume$                       \n\t"
        "    str %[next_pc], [%[p_saved_pc], #0]            \n\t"
        "                                                   \n\t"
        "restore_incoming$:                                 \n\t"
        "    /* Restore saved CPSR into SPSR */             \n\t"
        "    ldr %[cpsr_temp], [%[p_incoming_cpsr], #0]     \n\t"
        "    msr spsr, %[cpsr_temp]                         \n\t"
        "                                                   \n\t"
        "    /* Atomically load normal regs and     */      \n\t"
        "    /* transfer SPSR into CPSR             */      \n\t"
        "    ldm %[p_incoming], {r0 - r15}^                 \n\t"
        "                                                   \n\t"
        "resume$:                                           \n\t"
        "    nop                                            \n\t"
        "    nop                                            \n\t"
        "                                                   \n\t"
        : [next_pc] "+r" (next_pc),
          [cpsr_temp] "+r" (cpsr_temp)
        : [p_outgoing] "r" (&outgoing->registers),
          [p_incoming] "r" (&incoming->registers),
          [p_saved_pc] "r" (&outgoing->registers[REGISTER_INDEX_PC]),
          [p_outgoing_cpsr] "r" (&outgoing->registers[REGISTER_INDEX_PSR]),
          [p_incoming_cpsr] "r" (&incoming->registers[REGISTER_INDEX_PSR]),
          [prev_irq] "r" (prev_irq_status.cpsr_interrupt_flags),
          [int_bits] "i" (SETBIT(ARM_CPSR_I_BIT) | SETBIT(ARM_CPSR_F_BIT))
        : "memory"
    );
}

static void thread_entry (ThreadFunc func, void * param)
{
    func(param);

    THREAD_CURRENT()->state = THREAD_STATE_FINISHED;

    if (THREAD_CURRENT()->joiner != NULL) {
        ThreadAddReady(THREAD_CURRENT()->joiner);
    }

    ThreadYieldNoRequeue();
}

struct Thread * ThreadCreate (ThreadFunc body, void * param)
{
    unsigned int    i;
    struct Page *   stack_page;
    struct Thread * descriptor;

    stack_page = VmPageAlloc();

    if (!stack_page) {
        /* No memory available to allocate stack */
        return NULL;
    }

    /*
    Carve the thread struct out of the beginning (high addresses)
    of the kernel stack.
    */
    descriptor = THREAD_STRUCT_FROM_SP(stack_page->base_address);

    for (i = 0; i < sizeof(descriptor->registers) / sizeof(descriptor->registers[0]); ++i)
    {
        descriptor->registers[i] = 0;
    }

    descriptor->kernel_stack.ceiling = descriptor;
    descriptor->kernel_stack.base = (void *)stack_page->base_address;
    descriptor->kernel_stack.page = stack_page;
    descriptor->process = THREAD_CURRENT()->process;
    INIT_LIST_HEAD(&descriptor->queue_link);
    descriptor->state = THREAD_STATE_READY;
    descriptor->joiner = NULL;
    descriptor->assigned_priority = THREAD_PRIORITY_NORMAL;
    descriptor->effective_priority = THREAD_PRIORITY_NORMAL;

    /* Initially only the program and stack counter matter. */
    descriptor->registers[REGISTER_INDEX_SP] = (uint32_t)descriptor->kernel_stack.ceiling;

    /* Set up the entrypoint function with argument values */
    descriptor->registers[REGISTER_INDEX_PC]    = (uint32_t)thread_entry;
    descriptor->registers[REGISTER_INDEX_ARG0]  = (uint32_t)body;
    descriptor->registers[REGISTER_INDEX_ARG1]  = (uint32_t)param;

    /* Thread is initially running in kernel mode */
    asm volatile (
        "mov %[cpsr], %[svc_mode_bits]          \n\t"
        : [cpsr] "=r" (descriptor->registers[REGISTER_INDEX_PSR])
        : [svc_mode_bits] "i" (ARM_SVC_MODE_BITS)
    );

    /* Yield immediately to new thread so that it gets initialized */
    ThreadSwitch(THREAD_CURRENT(), descriptor, (ThreadSwitchPreFunc)ThreadAddReady, THREAD_CURRENT());

    return descriptor;
}

void ThreadJoin (struct Thread * thread)
{
    assert(THREAD_CURRENT() != thread);
    assert(thread->joiner == NULL);

    thread->joiner = THREAD_CURRENT();

    while (thread->state != THREAD_STATE_FINISHED) {
        ThreadYieldNoRequeue();
    }

    if (thread->kernel_stack.page != NULL) {
        VmPageFree(thread->kernel_stack.page);
    }
}

void ThreadSetEffectivePriority (struct Thread * thread, ThreadPriority priority)
{
    thread->effective_priority = priority;
}

void ThreadAddReady (struct Thread * thread)
{
    SpinlockLock(&ready_queue_lock);
    list_add_tail(&thread->queue_link, queue_for_thread(thread));
    thread->state = THREAD_STATE_READY;
    SpinlockUnlock(&ready_queue_lock);
}

void ThreadAddReadyFirst (struct Thread * thread)
{
    SpinlockLock(&ready_queue_lock);
    list_add(&thread->queue_link, queue_for_thread(thread));
    thread->state = THREAD_STATE_READY;
    SpinlockUnlock(&ready_queue_lock);
}

struct Thread * ThreadDequeueReady (void)
{
    struct Thread * next;

    SpinlockLock(&ready_queue_lock);

    if (!list_empty(&io_ready_queue)) {
        next = list_first_entry(&io_ready_queue, struct Thread, queue_link);
        list_del_init(&next->queue_link);
    }
    else if (!list_empty(&normal_ready_queue)) {
        next = list_first_entry(&normal_ready_queue, struct Thread, queue_link);
        list_del_init(&next->queue_link);
    } else {
        next = NULL;
    }

    SpinlockUnlock(&ready_queue_lock);

    return next;
}

void ThreadYieldNoRequeue (void)
{
    /* Pop off thread at front of run-queue */
    struct Thread * next = ThreadDequeueReady();

    /* Since we're not requeuing, there had better be somebody runnable */
    assert(next != NULL);

    ThreadSwitch(THREAD_CURRENT(), next, NULL, NULL);
}

void ThreadYieldWithRequeue (void)
{
    /* Pop off thread at front of run-queue */
    struct Thread * next = ThreadDequeueReady();

    /* Since we're requeuing, it's OK if there were no other runnable threads */
    if (next != NULL) {
        ThreadSwitch(THREAD_CURRENT(), next, (ThreadSwitchPreFunc)ThreadAddReady, THREAD_CURRENT());
    }
}

void ThreadYieldNoRequeueToSpecific (struct Thread * next)
{
    assert(list_empty(&next->queue_link));

    ThreadSwitch(THREAD_CURRENT(), next, NULL, NULL);
}

/**
 * Set to true by interrupt handlers when something has happened
 * that makes the scheduler algorithm need to be re-run at
 * the time that a syscall is being returned from.
 */
static bool         need_resched        = false;
static Spinlock_t   need_resched_lock   = SPINLOCK_INIT;

void ThreadSetNeedResched ()
{
    SpinlockLock(&need_resched_lock);
    need_resched = true;
    SpinlockUnlock(&need_resched_lock);
}

bool ThreadResetNeedResched ()
{
    bool ret;

    SpinlockLock(&need_resched_lock);
    ret = need_resched;
    need_resched = false;
    SpinlockUnlock(&need_resched_lock);

    return ret;
}

struct Thread * ThreadStructFromStackPointer (uint32_t sp)
{
    return THREAD_STRUCT_FROM_SP(sp);
}

struct Process * ThreadGetProcess (struct Thread * thread)
{
    return thread->process;
}

void ThreadSetStateReady (struct Thread * thread)
{
    thread->state = THREAD_STATE_READY;
}

void ThreadSetStateRunning (struct Thread * thread)
{
    thread->state = THREAD_STATE_RUNNING;
}
