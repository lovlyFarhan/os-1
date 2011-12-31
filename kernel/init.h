#ifndef __INIT_H__
#define __INIT_H__

#include <stdint.h>

#include <sys/decls.h>

BEGIN_DECLS

extern uint8_t init_stack[];
extern uint8_t * init_stack_ceiling;

extern void Init (void)
    __attribute__ ((noreturn));

END_DECLS

#endif /* __INIT_H__ */
