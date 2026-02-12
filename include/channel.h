#ifndef CHANNEL_H
#define CHANNEL_H

#include "ds/vec.h"
#include <stdbool.h>

#ifndef __EMSCRIPTEN__
#include <pthread.h>
#endif

/* Forward declare LatValue to avoid circular includes */
struct LatValue;

typedef struct LatChannel {
    LatVec  buffer;       /* ring buffer of LatValue */
    bool    closed;
    size_t  refcount;
#ifndef __EMSCRIPTEN__
    pthread_mutex_t mutex;
    pthread_cond_t  cond_notempty;
#endif
} LatChannel;

/* Create a new channel (refcount=1) */
LatChannel *channel_new(void);

/* Increment refcount */
void channel_retain(LatChannel *ch);

/* Decrement refcount; frees when it reaches 0 */
void channel_release(LatChannel *ch);

/* Send a value into the channel. Returns false if closed. */
bool channel_send(LatChannel *ch, struct LatValue val);

/* Receive a value from the channel. Blocks until a value is available or
 * the channel is closed. Sets *ok to false if channel is closed+empty. */
struct LatValue channel_recv(LatChannel *ch, bool *ok);

/* Close the channel (no more sends allowed) */
void channel_close(LatChannel *ch);

#endif /* CHANNEL_H */
