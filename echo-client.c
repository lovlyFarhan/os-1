#include <muos/procmgr.h>
#include <muos/syscall.h>
#include <muos/message.h>
#include <muos/naming.h>
#include <muos/process.h>

#define N_ELEMENTS(_array)  \
    (                       \
    sizeof(_array) /        \
    sizeof(_array[0])       \
    )

int main () {
    struct iovec msgv[3];
    struct iovec replyv[3];
    /* Send message to echo server */
    char msg[] = "Artoo";
    char reply[sizeof(msg)];
    int echoCon = NameOpen("/dev/echo");

    /*
    Just for fun, fragment up the message to exercise the
    vectored message passing.
    */
    msgv[0].iov_base = msg;
    msgv[0].iov_len = 1;
    msgv[1].iov_base = msg + msgv[0].iov_len;
    msgv[1].iov_len = 1;
    msgv[2].iov_base = msg + msgv[0].iov_len + msgv[1].iov_len;
    msgv[2].iov_len = sizeof(msg) - msgv[0].iov_len - msgv[1].iov_len;

    replyv[0].iov_base = reply;
    replyv[0].iov_len = 2;
    replyv[1].iov_base = reply + replyv[0].iov_len;
    replyv[1].iov_len = 2;
    replyv[2].iov_base = reply + replyv[0].iov_len + replyv[1].iov_len;
    replyv[2].iov_len = sizeof(reply) - replyv[0].iov_len - replyv[1].iov_len;
    MessageSendV(echoCon, msgv, N_ELEMENTS(msgv), replyv, N_ELEMENTS(replyv));

    /* Terminate */
    return 0;
}
