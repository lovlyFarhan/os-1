#ifndef __PROCMGR_H__
#define __PROCMGR_H__

#include <sys/compiler.h>
#include <sys/decls.h>
#include <sys/procmgr.h>

#include <kernel/message.hpp>

BEGIN_DECLS

#define PROC_MGR_OPERATION(_type, _func)                                    \
                                                                            \
    __attribute__((constructor))                                            \
    static void COMPILER_PREPROC_CONCAT(OperationRegistrar, __LINE__) ()    \
    {                                                                       \
        ProcMgrRegisterMessageHandler(_type, _func);                        \
    }

typedef void (*ProcMgrOperationFunc) (
        struct Message * message,
        const struct ProcMgrMessage * buf
        );

void ProcMgrRegisterMessageHandler (
        enum ProcMgrMessageType type,
        ProcMgrOperationFunc func
        );

ProcMgrOperationFunc ProcMgrGetMessageHandler (enum ProcMgrMessageType type);

END_DECLS

#endif /* __PROCMGR_H__ */