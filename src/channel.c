#include "channel.h"
#include "value.h"
#include <stdlib.h>
#include <string.h>

LatChannel *channel_new(void) {
    LatChannel *ch = calloc(1, sizeof(LatChannel));
    if (!ch) return NULL;
    ch->buffer = lat_vec_new(sizeof(LatValue));
    ch->closed = false;
    ch->refcount = 1;
#ifndef __EMSCRIPTEN__
    pthread_mutex_init(&ch->mutex, NULL);
    pthread_cond_init(&ch->cond_notempty, NULL);
#endif
    return ch;
}

void channel_retain(LatChannel *ch) {
    __atomic_add_fetch(&ch->refcount, 1, __ATOMIC_SEQ_CST);
}

void channel_release(LatChannel *ch) {
    size_t prev = __atomic_sub_fetch(&ch->refcount, 1, __ATOMIC_SEQ_CST);
    if (prev == 0) {
        /* Free any remaining buffered values */
        for (size_t i = 0; i < ch->buffer.len; i++) {
            LatValue *v = lat_vec_get(&ch->buffer, i);
            value_free(v);
        }
        lat_vec_free(&ch->buffer);
#ifndef __EMSCRIPTEN__
        pthread_mutex_destroy(&ch->mutex);
        pthread_cond_destroy(&ch->cond_notempty);
#endif
        free(ch);
    }
}

bool channel_send(LatChannel *ch, LatValue val) {
#ifndef __EMSCRIPTEN__
    pthread_mutex_lock(&ch->mutex);
#endif
    if (ch->closed) {
#ifndef __EMSCRIPTEN__
        pthread_mutex_unlock(&ch->mutex);
#endif
        value_free(&val);
        return false;
    }
    lat_vec_push(&ch->buffer, &val);
#ifndef __EMSCRIPTEN__
    pthread_cond_signal(&ch->cond_notempty);
    /* Wake any select waiters */
    for (LatSelectWaiter *w = ch->waiters; w; w = w->next) {
        pthread_mutex_lock(w->mutex);
        pthread_cond_signal(w->cond);
        pthread_mutex_unlock(w->mutex);
    }
    pthread_mutex_unlock(&ch->mutex);
#endif
    return true;
}

LatValue channel_recv(LatChannel *ch, bool *ok) {
#ifndef __EMSCRIPTEN__
    pthread_mutex_lock(&ch->mutex);
    while (ch->buffer.len == 0 && !ch->closed) {
        pthread_cond_wait(&ch->cond_notempty, &ch->mutex);
    }
#endif
    if (ch->buffer.len == 0) {
        /* closed + empty */
#ifndef __EMSCRIPTEN__
        pthread_mutex_unlock(&ch->mutex);
#endif
        *ok = false;
        return value_unit();
    }
    /* Shift front element */
    LatValue val;
    memcpy(&val, lat_vec_get(&ch->buffer, 0), sizeof(LatValue));
    /* Shift remaining elements forward */
    if (ch->buffer.len > 1) {
        memmove(ch->buffer.data,
                (char *)ch->buffer.data + ch->buffer.elem_size,
                (ch->buffer.len - 1) * ch->buffer.elem_size);
    }
    ch->buffer.len--;
#ifndef __EMSCRIPTEN__
    pthread_mutex_unlock(&ch->mutex);
#endif
    *ok = true;
    return val;
}

void channel_close(LatChannel *ch) {
#ifndef __EMSCRIPTEN__
    pthread_mutex_lock(&ch->mutex);
#endif
    ch->closed = true;
#ifndef __EMSCRIPTEN__
    pthread_cond_broadcast(&ch->cond_notempty);
    /* Wake any select waiters */
    for (LatSelectWaiter *w = ch->waiters; w; w = w->next) {
        pthread_mutex_lock(w->mutex);
        pthread_cond_signal(w->cond);
        pthread_mutex_unlock(w->mutex);
    }
    pthread_mutex_unlock(&ch->mutex);
#endif
}

bool channel_try_recv(LatChannel *ch, LatValue *out, bool *closed_out) {
#ifndef __EMSCRIPTEN__
    pthread_mutex_lock(&ch->mutex);
#endif
    if (ch->buffer.len == 0) {
        if (closed_out) *closed_out = ch->closed;
#ifndef __EMSCRIPTEN__
        pthread_mutex_unlock(&ch->mutex);
#endif
        return false;
    }
    memcpy(out, lat_vec_get(&ch->buffer, 0), sizeof(LatValue));
    if (ch->buffer.len > 1) {
        memmove(ch->buffer.data,
                (char *)ch->buffer.data + ch->buffer.elem_size,
                (ch->buffer.len - 1) * ch->buffer.elem_size);
    }
    ch->buffer.len--;
    if (closed_out) *closed_out = false;
#ifndef __EMSCRIPTEN__
    pthread_mutex_unlock(&ch->mutex);
#endif
    return true;
}

void channel_add_waiter(LatChannel *ch, LatSelectWaiter *w) {
#ifndef __EMSCRIPTEN__
    pthread_mutex_lock(&ch->mutex);
    w->next = ch->waiters;
    ch->waiters = w;
    pthread_mutex_unlock(&ch->mutex);
#else
    (void)ch; (void)w;
#endif
}

void channel_remove_waiter(LatChannel *ch, LatSelectWaiter *w) {
#ifndef __EMSCRIPTEN__
    pthread_mutex_lock(&ch->mutex);
    LatSelectWaiter **pp = &ch->waiters;
    while (*pp) {
        if (*pp == w) { *pp = w->next; break; }
        pp = &(*pp)->next;
    }
    pthread_mutex_unlock(&ch->mutex);
#else
    (void)ch; (void)w;
#endif
}
