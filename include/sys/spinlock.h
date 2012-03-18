#ifndef __SPINLOCK_H__
#define __SPINLOCK_H__

#include <sys/atomic.h>
#include <sys/decls.h>
#include <sys/interrupts.h>

#ifdef __KERNEL__
    #include <kernel/assert.h>
#endif

BEGIN_DECLS

typedef struct
{
    uint32_t    lockval;
    IrqSave_t   irq_saved_state;
} Spinlock_t;

#define SPINLOCK_LOCKVAL_UNLOCKED   0
#define SPINLOCK_LOCKVAL_LOCKED     1

#ifdef __cplusplus
#   define SPINLOCK_INIT { lockval: SPINLOCK_LOCKVAL_UNLOCKED }
#else
#   define SPINLOCK_INIT { .lockval = SPINLOCK_LOCKVAL_UNLOCKED }
#endif

static inline void SpinlockInit (Spinlock_t * lock)
{
    /* Initially unlocked */
    lock->lockval = SPINLOCK_LOCKVAL_UNLOCKED;

    /* Don't care about saved IRQ status. It's overwritten on first use anyway. */
    lock->irq_saved_state = lock->irq_saved_state;
}

static inline void SpinlockLock (Spinlock_t * lock)
{
    #ifdef __KERNEL__
        assert(SPINLOCK_LOCKVAL_UNLOCKED == lock->lockval);
    #endif

    /* On UP systems, this line alone does all the real work. */
    lock->irq_saved_state = InterruptsDisable();

    while (!AtomicCompareAndExchange(&lock->lockval, SPINLOCK_LOCKVAL_UNLOCKED, SPINLOCK_LOCKVAL_LOCKED))
    {
    }
}

static inline void SpinlockUnlock (Spinlock_t * lock)
{
    #ifdef __KERNEL__
        assert(SPINLOCK_LOCKVAL_UNLOCKED != lock->lockval);
    #endif

    while (!AtomicCompareAndExchange(&lock->lockval, SPINLOCK_LOCKVAL_LOCKED, SPINLOCK_LOCKVAL_UNLOCKED))
    {
    }

    InterruptsRestore(lock->irq_saved_state);
}

END_DECLS

#endif /* __SPINLOCK_H__ */
