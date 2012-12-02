#include <sys/compiler.h>
#include <sys/procmgr.h>

#include <kernel/assert.h>
#include <kernel/exception.hpp>
#include <kernel/message.hpp>
#include <kernel/thread.hpp>

void ScheduleSelfAbort ()
{
    struct ProcMgrMessage message;
    Connection * connection;

    assert(THREAD_CURRENT()->process != NULL);
    connection = THREAD_CURRENT()->process->LookupConnection(PROCMGR_CONNECTION_ID);

    message.type = PROC_MGR_MESSAGE_SIGNAL;
    message.payload.signal.signalee_pid = THREAD_CURRENT()->process->GetId();

    IoBuffer chunk(&message, sizeof(message));
    IoBuffer reply(NULL, 0);
    connection->SendMessage(&chunk, 1, &reply, 0);

    assert(false);
}
