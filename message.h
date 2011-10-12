#ifndef __MESSAGE_H__
#define __MESSAGE_H__

#include <string.h>

#include "decls.h"

BEGIN_DECLS

struct Channel;
struct Connection;
struct Message;

extern struct Channel * ChannelAlloc (void);
extern void ChannelFree (struct Channel * channel);

extern struct Connection * Connect (struct Channel * channel);
extern void Disconnect (struct Connection * connection);

/**
 * @return number of bytes written to replybuf, or negative if error
 */
extern int MessageSend (
        struct Connection * connection,
        const void * msgbuf,
        size_t msgbuf_len,
        void * replybuf,
        size_t replybuf_len
        );

/**
 * @return the number of bytes written to msgbuf, or negative if error
 */
extern int MessageReceive (
        struct Channel * channel,
        struct Message ** context,
        void * msgbuf,
        size_t msgbuf_len
        );

/**
 * @return the number of bytes transmitted to replybuf, or negative if error
 */
extern int MessageReply (
        struct Message * context,
        void * replybuf,
        size_t replybuf_len
        );

END_DECLS

#endif /* __MESSAGE_H__ */