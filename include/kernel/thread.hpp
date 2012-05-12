#ifndef __THREAD_H__
#define __THREAD_H__

#include <sys/arch.h>
#include <sys/decls.h>

#include <kernel/list.hpp>
#include <kernel/process.hpp>

#define ALIGNED_THREAD_STRUCT_SIZE                                  \
    /* Padded out to multiple of 8 to preserve %sp requirements */  \
    (ALIGN(sizeof(struct Thread), 3))

#define THREAD_STRUCT_FROM_SP(_sp)                                  \
    (                                                               \
    (struct Thread *)                                               \
    (((_sp) & PAGE_MASK) + PAGE_SIZE - ALIGNED_THREAD_STRUCT_SIZE)  \
    )

/*
We leverage knowledge that the kernel stack is only one page long, to
be able to compute the address of the current thread's struct based
solely on the current stack pointer.

Don't blow your thread stack! This will return a bad result.
*/
#define THREAD_CURRENT() \
    (THREAD_STRUCT_FROM_SP(CURRENT_STACK_POINTER()))

BEGIN_DECLS

typedef enum
{
    THREAD_STATE_SEND,
    THREAD_STATE_REPLY,
    THREAD_STATE_RECEIVE,

    THREAD_STATE_READY,
    THREAD_STATE_RUNNING,

    THREAD_STATE_FINISHED,

    /* This isn't a state, just a way to programatically calculate */
    THREAD_STATE_COUNT,
} ThreadState;

typedef enum
{
    THREAD_PRIORITY_NORMAL  = 0,
    THREAD_PRIORITY_IO,

    THREAD_PRIORITY_COUNT,
} ThreadPriority;

/*
The main control and saved-state for kernel threads. Each thread's
instance of this structure is housed inside the top of the VM page
used for the thread's stack. This avoids object allocations and also
makes deducing the current thread easy: just compute the right offset
in the page containing the current stack pointer.
*/
struct Thread
{
    uint32_t    registers[REGISTER_COUNT];

    struct
    {
        void *  ceiling;
        void *  base;

        /* If non-NULL, stack was dynamically allocated */
        Page * page;
    } kernel_stack;

    ThreadState state;

    struct Process * process;

    /* For use in scheduling queues. */
    ListElement queue_link;

    /* Thread that will wait for and reap this one */
    struct Thread * joiner;

    /* "Natural" priority of this thread. */
    ThreadPriority assigned_priority;

    /* Ceiling of the priorities of all threads blocked by this one. */ 
    ThreadPriority effective_priority;
};

typedef void (*ThreadFunc)(void * param);

extern struct Thread * ThreadCreate (
        ThreadFunc body,
        void * param
        );

/**
 * For use in implementing priority inheritance; install an artifically higher
 * priority for this thread than its natural one.
 */
extern void ThreadSetEffectivePriority (struct Thread * thread, ThreadPriority priority);

/**
 * Deallocates resources used by @thread. Must not be called while @thread
 * is executing on the processor.
 */
extern void ThreadJoin (struct Thread * thread);

/**
 * Yield to some other runnable thread. Must not be called with interrupts
 * disabled.
 */
extern void ThreadYieldNoRequeue (void);

/**
 * Yield to some other runnable thread, and automatically mark the
 * current thread as ready-to-run.
 */
extern void ThreadYieldWithRequeue (void);

extern void ThreadAddReady (struct Thread * thread);

extern void ThreadAddReadyFirst (struct Thread * thread);

extern struct Thread * ThreadDequeueReady (void);

extern void ThreadSetNeedResched (void);

extern bool ThreadGetNeedResched (void);

extern bool ThreadResetNeedResched (void);

/*----------------------------------------------------------
Convenience routines for use from assembly code
----------------------------------------------------------*/

/**
 * A version of THREAD_STRUCT_FROM_SP(), but implemented as a symbol
 * for calling from places where macros aren't available (e.g., assembly).
 */
extern struct Thread * ThreadStructFromStackPointer (uint32_t sp);

/**
 * A symbol that fetches the value of the 'process' field on a Thread
 * object, but implemented as a symbol for uses where C structures'
 * field names can't be used (e.g., assembly).
 */
extern struct Process * ThreadGetProcess (struct Thread *);

extern void ThreadSetStateReady (struct Thread *);
extern void ThreadSetStateRunning (struct Thread *);
extern void ThreadSetStateSend (struct Thread *);
extern void ThreadSetStateReply (struct Thread *);
extern void ThreadSetStateReceive (struct Thread *);
extern void ThreadSetStateFinished (struct Thread *);

END_DECLS

#endif /* __THREAD_H__ */
