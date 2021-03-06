#ifndef __MUOS_SYSCALL_H__
#define __MUOS_SYSCALL_H__

#include <muos/decls.h>

BEGIN_DECLS

enum
{
    SYS_BASE = 0x100,

    SYS_CHANNEL_CREATE = SYS_BASE,
    SYS_CHANNEL_DESTROY,
    SYS_CONNECT,
    SYS_DISCONNECT,
    SYS_MSGSEND,
    SYS_MSGSENDV,
    SYS_MSGRECV,
    SYS_MSGRECVV,
    SYS_MSGREPLY,
    SYS_MSGREPLYV,
    SYS_MSGGETLEN,
    SYS_MSGREAD,
    SYS_MSGREADV,
};

/* Prototypes for userspace syscall stubs */
extern int syscall0 (unsigned int number);
extern int syscall1 (unsigned int number, int arg0);
extern int syscall2 (unsigned int number, int arg0, int arg1);
extern int syscall3 (unsigned int number, int arg0, int arg1, int arg2);
extern int syscall4 (unsigned int number, int arg0, int arg1, int arg2, int arg3);
extern int syscall5 (unsigned int number, int arg0, int arg1, int arg2, int arg3, int arg4);

END_DECLS

#endif /* __MUOS_SYSCALL_H__ */
