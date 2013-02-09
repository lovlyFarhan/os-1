#include <string.h>

#include <muos/arch.h>
#include <muos/array.h>
#include <muos/atomic.h>
#include <muos/elf.h>
#include <muos/error.h>
#include <muos/procmgr.h>

#include <kernel/assert.h>
#include <kernel/math.hpp>
#include <kernel/minmax.hpp>
#include <kernel/process.hpp>
#include <kernel/procmgr.hpp>
#include <kernel/ramfs.h>
#include <kernel/semaphore.hpp>
#include <kernel/thread.hpp>
#include <kernel/timer.hpp>
#include <kernel/tree-map.hpp>

/** Handed off between spawner and spawnee threads */
struct process_creation_context
{
    Thread      * caller;
    Process     * parent;
    Process     * created;
    const char  * executableName;
    Semaphore   * baton;
};

/** Allocates monotonically increasing process identifiers */
static Pid_t get_next_pid (void);

SyncSlabAllocator<Process> Process::sSlab;

Process::Process (char const aComm[], Process * aParent)
    : mAddressSpace(new AddressSpace())
    , entry(0)
    , thread(NULL)
    , next_chid(FIRST_CHANNEL_ID)
    , next_coid(FIRST_CONNECTION_ID)
    , next_msgid(1)
    , next_interrupt_handler_id(1)
    , next_child_wait_handler_id(1)
    , mParent(aParent)
{
    this->pid = get_next_pid();

    /* Record our name */
    strncpy(comm, aComm, sizeof(comm));

    SpinlockInit(&this->lock);

    this->id_to_channel_map    = new IdToChannelMap_t(IdToChannelMap_t::SignedIntCompareFunc);
    this->id_to_connection_map = new IdToConnectionMap_t(IdToConnectionMap_t::SignedIntCompareFunc);
    this->id_to_message_map    = new IdToMessageMap_t(IdToMessageMap_t::SignedIntCompareFunc);
    this->id_to_interrupt_handler_map   = new IdToInterruptHandlerMap_t(IdToInterruptHandlerMap_t::SignedIntCompareFunc);

    if (aParent) {
        aParent->mAliveChildren.Append(this);
    }
}

static void ForeachMessage (
        RawTreeMap::Key_t key,
        RawTreeMap::Value_t value,
        void * ignored
        )
{
    Message * message = static_cast<Message *>(value);

    // Only synchronous messages end up registered in the id_to_message_map.
    //
    // So, each of these has a client send-blocked on it. Just reply back
    // with a failure code. The client will be responsible for deallocating
    // the message instance.
    message->Reply(ERROR_NO_SYS, IoBuffer::GetEmpty());
}

static void DisposeConnection (
        RawTreeMap::Key_t key,
        RawTreeMap::Value_t value,
        void * ignored
        )
{
    Connection * connection = static_cast<Connection *>(value);

    // Drop all internal references
    connection->Dispose();

    // Little dance to avoid making the manual Unref() call below
    // be the one to drop the final reference.
    RefPtr<Connection> deleter(connection);
    connection->Unref();
    deleter.Reset();
}

static void DisposeChannel (
        RawTreeMap::Key_t key,
        RawTreeMap::Value_t value,
        void * ignored
        )
{
    Channel * channel = static_cast<Channel *>(value);

    // Drop all internal references
    channel->Dispose();

    // Little dance to avoid making the manual Unref() call below
    // be the one to drop the final reference.
    RefPtr<Channel> deleter(channel);
    channel->Unref();
    deleter.Reset();
}

static void DisposeInterruptHandler (
        RawTreeMap::Key_t key,
        RawTreeMap::Value_t value,
        void * ignored
        )
{
    UserInterruptHandler * handler = static_cast<UserInterruptHandler *>(value);

    // Little dance to avoid making the manual Unref() call below
    // be the one to drop the final reference
    RefPtr<UserInterruptHandler> deleter(handler);
    handler->Unref();
    handler->Dispose();

    InterruptDetachUserHandler(deleter);
    deleter.Reset();
}

Process::~Process ()
{
    assert(GetId() != PROCMGR_PID + 1);

    // Reassign all children to the init process
    while (!mAliveChildren.Empty())
    {
        Process * child = mAliveChildren.PopFirst();
        Process * init = Process::Lookup(PROCMGR_PID + 1);

        child->mParent = init;
        init->mAliveChildren.Append(child);
    }

    while (!mDeadChildren.Empty())
    {
        Process * child = mDeadChildren.PopFirst();
        Process * init = Process::Lookup(PROCMGR_PID + 1);

        child->mParent = init;
        init->mDeadChildren.Append(child);

        RefPtr<Reaper> handler = init->GetReaperForChild(child->GetId());
        if (handler) {
            init->TryReapChildren(handler);
        }
    }

    /*
    Free all connections owned by process. Internally, the destructor for
    the connection object will free any messages that have been queued
    for sending but aren't yet received by a server.
    */
    this->id_to_connection_map->Foreach (DisposeConnection, NULL);

    this->id_to_channel_map->Foreach (DisposeChannel, NULL);

    /* Free all messages that the process has received but not yet responded to */
    this->id_to_message_map->Foreach (ForeachMessage, NULL);

    /* Free and unregister all interrupt handlers installed by the process */
    this->id_to_interrupt_handler_map->Foreach (DisposeInterruptHandler, NULL);

    /* Free all the child-termination handlers on this process */
    while (!this->mReapers.Empty()) {
        RefPtr<Reaper> reaper = mReapers.PopFirst();
        reaper.Reset();
    }
}

Process * Process::execIntoCurrent (const char executableName[],
                                    Process * aParent) throw (std::bad_alloc)
{
    Process * p;
    RamFsBufferPtr image;
    size_t image_len;
    const Elf32_Ehdr * hdr;
    unsigned int i;
    Connection_t procmgr_coid;
    Process * procmgr;
    RefPtr<Connection> procmgr_con;
    RefPtr<Channel> procmgr_chan;

    if (!RamFsGetImage(executableName, &image, &image_len)) {
        return NULL;
    }

    hdr = (const Elf32_Ehdr *)image;

    if(hdr->e_ident[EI_MAG0] != ELFMAG0 || hdr->e_ident[EI_MAG1] != ELFMAG1 ||
       hdr->e_ident[EI_MAG2] != ELFMAG2 || hdr->e_ident[EI_MAG3] != ELFMAG3) {
        return NULL;
    }

    if ((hdr->e_entry == 0) || (hdr->e_type != ET_EXEC) ||
        (hdr->e_machine != EM_ARM) || (hdr->e_phoff == 0)) {
        return NULL;
    }

    try {
        p = new Process(executableName, aParent);
    } catch (std::bad_alloc) {
        return NULL;
    }

    /* Record Pid */
    Process::Register(p->pid, p);

    /* Okay. Save reference to this process object into the current thread */
    p->thread = THREAD_CURRENT();
    THREAD_CURRENT()->process = p;

    /*
    Make sure that pagetable installation is flushed out to memory before
    making any use of it. This will make sure that an inconveniently timed
    preemption won't see the Process object in a moment when the fields
    modified above haven't been spilled back yet.
    */
    AtomicCompilerMemoryBarrier();

    // Okay, now it's safe to use the translation table.
    TranslationTable::SetUser(*p->mAddressSpace->GetPageTable());

    p->entry = hdr->e_entry;

    for (i = 0; i < hdr->e_phnum; i++) {
        const Elf32_Phdr * phdr;

        phdr = (const Elf32_Phdr *)
                    (image + hdr->e_phoff + i * hdr->e_phentsize);

        if (phdr->p_type == PT_LOAD) {

            /* Handle segments that don't line up on page boundaries */
            VmAddr_t base = phdr->p_vaddr & PAGE_MASK;
            size_t length = phdr->p_memsz + (phdr->p_vaddr - base);

            /* All the address space of the process must be in user memory range */
            assert(base + length <= KERNEL_MODE_OFFSET);

            if (!p->mAddressSpace->CreateBackedMapping(
                    base,
                    Math::RoundUp(length, PAGE_SIZE)))
            {
                // Requested address conflicted withs something already
                // there.
                assert(false);
                goto free_process;
            }

            /* With VM configured, simple memcpy() to load the contents. */

            /* ... first the explicitly initialized part */
            memcpy(
                (void *)phdr->p_vaddr,
                image + phdr->p_offset,
                phdr->p_filesz
                );

            /* ... now fill the zero-init part */
            if (phdr->p_filesz < phdr->p_memsz) {
                memset(
                    (void *)(phdr->p_vaddr + phdr->p_filesz),
                    0,
                    phdr->p_memsz - phdr->p_filesz
                    );
            }
        }
    }

    /* Establish the connection to the Process Manager's single channel. */
    procmgr = Lookup(PROCMGR_PID);
    assert(procmgr != NULL);
    procmgr_chan = procmgr->LookupChannel(FIRST_CHANNEL_ID);
    assert(procmgr_chan);

    try {
        procmgr_con.Reset(new Connection(procmgr_chan));
    } catch (std::bad_alloc) {
        goto free_process;
    }

    procmgr_coid = p->RegisterConnection(procmgr_con);
    
    if (procmgr_coid < 0) {
        procmgr_con.Reset();
        goto free_process;
    }

    assert(procmgr_coid == PROCMGR_CONNECTION_ID);
    
    return p;

free_process:

    assert(false);
    delete p;
    return NULL;
}

void Process::UserProcessThreadBody (void * pProcessCreationContext)
{
    struct process_creation_context * context;
    Process * p;

    context  = (struct process_creation_context *)pProcessCreationContext;
    context->created = p = Process::execIntoCurrent(context->executableName, context->parent);

    /* Release the spawner now that we have the resulting Process object */
    context->baton->Up();

    if (p) {

        /* Jump into the new process */
        uint32_t spsr;

        assert(!InterruptsDisabled());

        /*
        Need to make sure that no context switch comes along and
        trashes the value we're setting up in SPSR.
        */
        InterruptsDisable();

        /* Configure the SPSR to be user-mode execution */
        asm volatile(
            "mrs %[spsr], spsr              \n\t"
            "mov %[spsr], %[usr_mode_bits]  \n\t"
            "msr spsr, %[spsr]              \n\t"
            : [spsr] "=r" (spsr)
            : [usr_mode_bits] "i" (ARM_PSR_MODE_USR_BITS)
        );

        /* Jump to user mode (SPSR becomes the user-mode CPSR */
        asm volatile(
            "mov lr, %[user_pc]     \n\t"
            "mov r0, #0             \n\t"
            "mov r1, #0             \n\t"
            "mov r2, #0             \n\t"
            "mov r3, #0             \n\t"
            "mov r4, #0             \n\t"
            "mov r5, #0             \n\t"
            "mov r6, #0             \n\t"
            "mov r7, #0             \n\t"
            "mov r8, #0             \n\t"
            "mov r9, #0             \n\t"
            "mov r10, #0            \n\t"
            "mov r11, #0            \n\t"
            "mov r12, #0            \n\t"
            "movs pc, lr            \n\t"
            :
            : [user_pc] "r" (p->entry)
        );

        /* Unreachable */
        assert(false);
    }

    /*
    If control reaches here, it's because this thread has no more purpose.

    The resources for the process couldn't be loaded, and a return value has
    already been created back to the spawner.

    Just let this thread fall off the end of itself and be reclaimed.
    */
}

Process * Process::Create (const char aExecutableName[],
                           Process * aParent)
{
    struct process_creation_context context;
    Thread * t;

    Semaphore baton(0);

    if (GetManager() == NULL) {
        /* Something bad happened. Process manager wasn't spawned. */
        return NULL;
    }

    context.caller = THREAD_CURRENT();
    context.parent = aParent;
    context.created = NULL;
    context.executableName = aExecutableName;
    context.baton = &baton;

    /* Resulting process object will be stored into context->created */
    t = Thread::Create(UserProcessThreadBody, &context);

    /* Forked thread will wake us back up when the process creation is done */
    baton.Down();

    if (context.created != 0) {
        return context.created;
    } else {
        t->Join();
        return 0;
    }
}

static Pid_t get_next_pid ()
{
    static Pid_t counter = PROCMGR_PID;

    return counter++;
}

Spinlock_t Process::sPidMapSpinlock = SPINLOCK_INIT;

Process::PidMap_t Process::sPidMap(
        Process::PidMap_t::SignedIntCompareFunc
        );

Process * Process::Register (Pid_t key, Process * process)
{
    Process * ret;

    SpinlockLock(&sPidMapSpinlock);
    ret = sPidMap.Insert(key, process);
    SpinlockUnlock(&sPidMapSpinlock);

    return ret;
}

Process * Process::Remove (Pid_t key)
{
    Process * ret;

    SpinlockLock(&sPidMapSpinlock);
    ret = sPidMap.Remove(key);
    SpinlockUnlock(&sPidMapSpinlock);

    return ret;
}

Process * Process::Lookup (Pid_t pid)
{
    Process * ret;

    SpinlockLock(&sPidMapSpinlock);
    ret = sPidMap.Lookup(pid);
    SpinlockUnlock(&sPidMapSpinlock);

    return ret;
}

Pid_t Process::GetId ()
{
    return this->pid;
}

Thread * Process::GetThread ()
{
    return this->thread;
}

Process * Process::GetParent ()
{
    return mParent;
}

RefPtr<Reaper> Process::GetReaperForChild (Pid_t aChild)
{
    typedef RefList<Reaper, &Reaper::mLink> List_t;

    for (List_t::Iterator i = mReapers.Begin(); i; ++i) {
        if (i->Handles(aChild)) {
            return *i;
        }
    }

    return RefPtr<Reaper>();
}

typedef union
{
    struct ProcMgrMessage sync;
    struct Pulse async;
} MsgType;

void Process::ManagerThreadBody (void * pProcessCreationContext)
{
    struct process_creation_context * caller_context;
    Process * p;
    RefPtr<Channel> channel;

    MsgType                 msg;
    RefPtr<Message>         m;

    caller_context = (struct process_creation_context *)pProcessCreationContext;

    /* Allocate the singular channel on which the Process Manager listens for messages */
    try {
        channel.Reset(new Channel());
    } catch (std::bad_alloc) {
        /* This is unrecoverably bad. */
        assert(false);
        caller_context->created = NULL;
        caller_context->baton->Up();
        return;
    }

    try {
        caller_context->created = p = new Process("procmgr", NULL);
        p->mAddressSpace.Reset();
    } catch (std::bad_alloc a) {
        assert(false);
    }

    /* Allocate, assign, and record Pid */
    p->pid = PROCMGR_PID;
    Process::Register(p->pid, p);
    assert(Process::Lookup(p->pid) == p);

    /* Okay. Save reference to this process object into the current thread */
    p->thread = THREAD_CURRENT();
    THREAD_CURRENT()->process = p;

    /* Map the channel to a well-known integer identifier */
    Channel_t chid = p->RegisterChannel(channel);
    assert(chid == FIRST_CHANNEL_ID);

    /* Start periodic timer to use for pre-emption */
    Timer::StartPeriodic(5);
  
    /* Release the spawner now that we have the resulting Process object */
    caller_context->baton->Up();

    while (true) {
        size_t hdr_len = offsetof(struct ProcMgrMessage, type) + sizeof(msg.sync.type);
        size_t len = channel->ReceiveMessage(m, &msg, MAX(hdr_len, sizeof(struct Pulse)));

        if (!m) {
            // Pulse
            assert(msg.async.type == PULSE_TYPE_CHILD_FINISH);
            assert(len >= sizeof(struct Pulse));

            // PID of finished process is in pulse 'value' field
            Process * terminee = Process::Lookup(msg.async.value);

            // Wait until that process is totally done executing. This
            // amounts to making sure that its thread is finished
            // returning from the SendMessageAsync() call that injected
            // this message into our queue here.
            Thread::BeginTransaction();
            while (terminee->GetThread()->GetState() != Thread::STATE_FINISHED) {
                Thread::MakeReady(THREAD_CURRENT());
                Thread::RunNextThread();
            }
            Thread::EndTransaction();

            // Notify its parent
            terminee->GetParent()->ReportChildFinished(terminee);
        }
        else if (len >= hdr_len) {

            ProcMgrOperationFunc handler = ProcMgrGetMessageHandler(msg.sync.type);

            if (handler != NULL ) {
                handler(m);
            } else {
                m->Reply(ERROR_NO_SYS, IoBuffer::GetEmpty());
            }

        }
        else {
            /* Send back empty reply */
            m->Reply(ERROR_NO_SYS, IoBuffer::GetEmpty());
        }

        // Release ref on message so that it can be deallocated as soon
        // as sender wakes back up.
        m.Reset();
    }
}

static Process * managerProcess = NULL;

Process * Process::StartManager ()
{
    struct process_creation_context context;
    Semaphore baton(0);

    assert(managerProcess == NULL);

    context.caller = THREAD_CURRENT();
    context.created = NULL;
    context.executableName = NULL;
    context.baton = &baton;

    /* Resulting process object will be stored into context->created */
    Thread::Create(ManagerThreadBody, &context);

    /* Forked thread will wake us back up when the process creation is done */
    baton.Down();

    managerProcess = context.created;
    assert(managerProcess->GetId() == PROCMGR_PID);

    return managerProcess;
}

Process * Process::GetManager ()
{
    assert(managerProcess != NULL);
    return managerProcess;
}

Channel_t Process::RegisterChannel (RefPtr<Channel> c)
{
    Channel_t id = this->next_chid++;

    if (this->id_to_channel_map->Lookup(id) != NULL) {
        assert(false);
        return -ERROR_INVALID;
    }

    this->id_to_channel_map->Insert(id, *c);

    if (this->id_to_channel_map->Lookup(id) == *c) {
        c->Ref();
        return id;
    } else {
        return -ERROR_NO_MEM;
    }
}

int Process::UnregisterChannel (Channel_t id)
{
    Channel * c = this->id_to_channel_map->Remove(id);

    if (!c) {
        return -ERROR_INVALID;
    }

    RefPtr<Channel> deleter(c);
    c->Unref();
    deleter.Reset();

    return ERROR_OK;
}

RefPtr<Channel> Process::LookupChannel (Channel_t id)
{
    RefPtr<Channel> ret;

    Channel * value = this->id_to_channel_map->Lookup(id);

    if (value) {
        ret = value;
    }

    return ret;
}

Connection_t Process::RegisterConnection (RefPtr<Connection> c)
{
    Connection_t id = this->next_coid++;

    if (this->id_to_connection_map->Lookup(id) != NULL) {
        assert(false);
        return -ERROR_INVALID;
    }

    this->id_to_connection_map->Insert(id, *c);

    if (this->id_to_connection_map->Lookup(id) == *c) {
        c->Ref();
        return id;
    } else {
        return -ERROR_NO_MEM;
    }
}

int Process::UnregisterConnection (Connection_t id)
{
    Connection * c = this->id_to_connection_map->Remove(id);

    if (!c) {
        return -ERROR_INVALID;
    }

    RefPtr<Connection> deleter(c);
    c->Unref();
    deleter.Reset();

    return ERROR_OK;
}

RefPtr<Connection> Process::LookupConnection (Connection_t id)
{
    RefPtr<Connection> ret;

    Connection * value = this->id_to_connection_map->Lookup(id);

    if (value) {
        ret.Reset(value);
    }

    return RefPtr<Connection>(ret);
}

Message_t Process::RegisterMessage (RefPtr<Message> m)
{
    int msgid = this->next_msgid++;

    if (this->id_to_message_map->Lookup(msgid) != NULL) {
        assert(false);
        return -ERROR_INVALID;
    }

    this->id_to_message_map->Insert(msgid, *m);

    if (this->id_to_message_map->Lookup(msgid) == *m) {
        m->Ref();
        return msgid;
    } else {
        return -ERROR_NO_MEM;
    }
}

int Process::UnregisterMessage (Message_t id)
{
    Message * m = this->id_to_message_map->Remove(id);
    RefPtr<Message> deleter;

    if (!m) {
        return -ERROR_INVALID;
    }

    // Dtor will automatically invoke privileged Message dtor
    deleter.Reset(m);
    m->Unref();
    deleter.Reset();

    return ERROR_OK;
}

RefPtr<Message> Process::LookupMessage (Message_t id)
{
    RefPtr<Message> ret;

    Message * m = this->id_to_message_map->Lookup(id);

    if (m) {
        ret.Reset(m);
    }

    return ret;
}

int Process::RegisterInterruptHandler (RefPtr<UserInterruptHandler> h)
{
    int handler_id = this->next_interrupt_handler_id++;

    if (this->id_to_interrupt_handler_map->Lookup(handler_id) != NULL) {
        assert(false);
        return -ERROR_INVALID;
    }

    this->id_to_interrupt_handler_map->Insert(handler_id, *h);

    if (this->id_to_interrupt_handler_map->Lookup(handler_id) == *h) {
        h->Ref();
        return handler_id;
    } else {
        return -ERROR_NO_MEM;
    }
}

int Process::UnregisterInterruptHandler (int handler_id)
{
    UserInterruptHandler * h = this->id_to_interrupt_handler_map->Remove(handler_id);

    if (!h) {
        return -ERROR_INVALID;
    }

    RefPtr<UserInterruptHandler> deleter(h);
    h->Unref();
    deleter.Reset();

    return ERROR_OK;
}

RefPtr<UserInterruptHandler> Process::LookupInterruptHandler (int handler_id)
{
    RefPtr<UserInterruptHandler> ret;

    UserInterruptHandler * value = this->id_to_interrupt_handler_map->Lookup(handler_id);

    if (value) {
        ret.Reset(value);
    }

    return RefPtr<UserInterruptHandler>(ret);
}

int Process::RegisterReaper (RefPtr<Reaper> aReaper)
{
    int handler_id = this->next_child_wait_handler_id++;

    aReaper->mId = handler_id;

    mReapers.Append(aReaper);

    TryReapChildren(aReaper);

    return aReaper->mId;
}

int Process::UnregisterReaper (int handler_id)
{
    RefPtr<Reaper> r = LookupReaper(handler_id);

    if (r) {
        mReapers.Remove(r);
        return ERROR_OK;
    }
    else {
        return -ERROR_INVALID;
    }
}

RefPtr<Reaper> Process::LookupReaper (int handler_id)
{
    typedef RefList<Reaper, &Reaper::mLink> List_t;

    for (List_t::Iterator i = mReapers.Begin(); i; ++i) {
        if (i->mId == handler_id) {
            return *i;
        }
    }

    return RefPtr<Reaper>();
}

char const * Process::GetName ()
{
    return this->comm;
}

TranslationTable * Process::GetTranslationTable ()
{
    if (this->mAddressSpace) {
        return *this->mAddressSpace->GetPageTable();
    }
    else {
        return NULL;
    }
}

AddressSpace * Process::GetAddressSpace ()
{
    return *this->mAddressSpace;
}

TranslationTable * ProcessGetTranslationTable (Process * p)
{
    return p->GetTranslationTable();
}

void Process::TryReapChildren (RefPtr<Reaper> aReaper)
{
    typedef List<Process, &Process::mChildrenLink> List_t;

    for (List_t::Iterator i = mDeadChildren.Begin(); i; ++i) {
        if (aReaper->Handles(i->GetId()) && aReaper->mCount > 0) {
            aReaper->mCount--;
            ReapChild(*i, aReaper->mConnection);
        }
    }
}

void Process::ReapChild (Process * aChild, RefPtr<Connection> aConnection)
{
    Pid_t child_pid = aChild->GetId();
    Thread * thread = aChild->GetThread();

    Remove(child_pid);
    mDeadChildren.Remove(aChild);

    delete aChild;
    thread->process = NULL;
    thread->Join();

    aConnection->SendMessageAsync(PULSE_TYPE_CHILD_FINISH, child_pid);
}

void Process::ReportChildFinished (Process * aChild)
{
    Pid_t child_pid = aChild->GetId();

    mAliveChildren.Remove(aChild);
    mDeadChildren.Append(aChild);

    RefPtr<Reaper> handler = GetReaperForChild(child_pid);

    if (handler && handler->Handles(child_pid) && handler->mCount > 0) {
        handler->mCount--;
        ReapChild(aChild, handler->mConnection);
    }

}

/**
 * Handler for PROC_MGR_MESSAGE_EXIT.
 */
static void HandleExitMessage (RefPtr<Message> message)
{
    Thread * sender = message->GetSender();

    /* Syscalls are always invoked by processes */
    assert(sender->process != NULL);

    /*
    Syscall entrypoint code will terminate process in response
    to the special ERROR_EXITING return code.
    */
    message->Reply(ERROR_EXITING, IoBuffer::GetEmpty());
}

/**
 * Handler for PROC_MGR_MESSAGE_SIGNAL
 */
static void HandleSignalMessage (RefPtr<Message> message)
{
    struct ProcMgrMessage buf;

    ssize_t msg_len = PROC_MGR_MSG_LEN(signal);
    ssize_t actual_len = message->Read(0, &buf, msg_len);

    if (actual_len != msg_len) {
        message->Reply(ERROR_INVALID, IoBuffer::GetEmpty());
    }
    else {
        Thread * sender = message->GetSender();
        Process * senderProcess = sender->process;
        Process * signalee = Process::Lookup(buf.payload.signal.signalee_pid);

        if (signalee == senderProcess) {
            /*
            Special return code; syscall framework will terminate
            caller upon return.
            */
            message->Reply(ERROR_EXITING, IoBuffer::GetEmpty());
        } else {
            // TODO: wake up signalle from any blocking sleeps and
            // allow it to unwind its stack back down to syscall entry,
            // where it will be reaped just as though it had called
            // Exit()
            assert(false);
            message->Reply(ERROR_INVALID, IoBuffer::GetEmpty());
        }
    }
}

PROC_MGR_OPERATION(PROC_MGR_MESSAGE_EXIT, HandleExitMessage)
PROC_MGR_OPERATION(PROC_MGR_MESSAGE_SIGNAL, HandleSignalMessage)
