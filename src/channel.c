#include "channel.h"
#include "value.h"
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef __EMSCRIPTEN__
/* Per-channel waiter node.
 *
 * A single `select` over several channels registers ONE caller-owned
 * LatSelectWaiter on every channel at once. The public LatSelectWaiter has a
 * single `next` pointer, so threading that one node through multiple channels'
 * waiter lists makes the lists share/overwrite each other's links — corrupting
 * them (lost wakeups / select hangs). To avoid that, each channel keeps its own
 * internally-allocated node: it records the owning waiter (for removal identity)
 * plus a copy of that waiter's wakeup mutex/cond, and owns a `next` that belongs
 * solely to this channel's list. These nodes live behind ch->waiters (declared
 * LatSelectWaiter* in the header) and are only ever touched here in channel.c. */
typedef struct ChWaiterNode {
    LatSelectWaiter *owner;
    pthread_mutex_t *mutex;
    pthread_cond_t *cond;
    struct ChWaiterNode *next;
} ChWaiterNode;
#endif

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

void channel_retain(LatChannel *ch) { __atomic_add_fetch(&ch->refcount, 1, __ATOMIC_SEQ_CST); }

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
    /* Detach the value from the sending thread's local heap so it survives the
     * thread's teardown — channels are drained by another thread, which would
     * otherwise read freed memory (use-after-free) and double-free on cleanup.
     * The sender-owned original is released into the sender's own heap. */
    LatValue owned = value_detach(&val);
    value_free(&val);
#ifndef __EMSCRIPTEN__
    pthread_mutex_lock(&ch->mutex);
#endif
    if (ch->closed) {
#ifndef __EMSCRIPTEN__
        pthread_mutex_unlock(&ch->mutex);
#endif
        value_free(&owned);
        return false;
    }
    lat_vec_push(&ch->buffer, &owned);
#ifndef __EMSCRIPTEN__
    pthread_cond_signal(&ch->cond_notempty);
    /* Wake any select waiters */
    for (ChWaiterNode *w = (ChWaiterNode *)ch->waiters; w; w = w->next) {
        pthread_mutex_lock(w->mutex);
        w->owner->notified = true;
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
    while (ch->buffer.len == 0 && !ch->closed) { pthread_cond_wait(&ch->cond_notempty, &ch->mutex); }
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
        memmove(ch->buffer.data, (char *)ch->buffer.data + ch->buffer.elem_size,
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
    for (ChWaiterNode *w = (ChWaiterNode *)ch->waiters; w; w = w->next) {
        pthread_mutex_lock(w->mutex);
        w->owner->notified = true;
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
        memmove(ch->buffer.data, (char *)ch->buffer.data + ch->buffer.elem_size,
                (ch->buffer.len - 1) * ch->buffer.elem_size);
    }
    ch->buffer.len--;
    if (closed_out) *closed_out = false;
#ifndef __EMSCRIPTEN__
    pthread_mutex_unlock(&ch->mutex);
#endif
    return true;
}

bool channel_add_waiter(LatChannel *ch, LatSelectWaiter *w) {
#ifndef __EMSCRIPTEN__
    /* Allocate a node private to THIS channel so the owner's single `next`
     * pointer is never shared between channel lists (see ChWaiterNode above). */
    ChWaiterNode *node = malloc(sizeof(ChWaiterNode));
    if (!node) return false;
    node->owner = w;
    node->mutex = w->mutex;
    node->cond = w->cond;
    pthread_mutex_lock(&ch->mutex);
    node->next = (ChWaiterNode *)ch->waiters;
    ch->waiters = (LatSelectWaiter *)node;
    pthread_mutex_unlock(&ch->mutex);
    return true;
#else
    (void)ch;
    (void)w;
    return false;
#endif
}

void channel_remove_waiter(LatChannel *ch, LatSelectWaiter *w) {
#ifndef __EMSCRIPTEN__
    pthread_mutex_lock(&ch->mutex);
    /* Drop every node this channel holds for the owning waiter (a select may
     * register the same owner more than once, e.g. two arms on one channel). */
    ChWaiterNode *head = (ChWaiterNode *)ch->waiters;
    ChWaiterNode *prev = NULL;
    ChWaiterNode *cur = head;
    while (cur) {
        ChWaiterNode *next = cur->next;
        if (cur->owner == w) {
            if (prev) prev->next = next;
            else head = next;
            free(cur);
        } else {
            prev = cur;
        }
        cur = next;
    }
    ch->waiters = (LatSelectWaiter *)head;
    pthread_mutex_unlock(&ch->mutex);
#else
    (void)ch;
    (void)w;
#endif
}

bool channel_select(LatChannel **channels, const uint8_t *flags, size_t count, long timeout_ms, size_t *selected_out,
                    LatValue *received_out) {
    *selected_out = SIZE_MAX;
    *received_out = value_unit();

#ifdef __EMSCRIPTEN__
    (void)channels;
    (void)flags;
    (void)count;
    (void)timeout_ms;
    return false;
#else
    int default_arm = -1;
    int timeout_arm = -1;
    size_t channel_count = 0;
    size_t *indices = count ? malloc(count * sizeof(size_t)) : NULL;
    if (count && !indices) return false;

    for (size_t i = 0; i < count; i++) {
        if (flags[i] & LAT_SELECT_DEFAULT) default_arm = (int)i;
        else if (flags[i] & LAT_SELECT_TIMEOUT) timeout_arm = (int)i;
        else indices[channel_count++] = i;
    }
    for (size_t i = channel_count; i > 1; i--) {
        size_t j = (size_t)rand() % i;
        size_t tmp = indices[i - 1];
        indices[i - 1] = indices[j];
        indices[j] = tmp;
    }

    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
    LatSelectWaiter waiter = {.mutex = &mutex, .cond = &cond, .notified = false, .next = NULL};
    struct timespec deadline = {0};
    if (timeout_ms >= 0) {
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += timeout_ms / 1000;
        deadline.tv_nsec += (timeout_ms % 1000) * 1000000L;
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000L;
        }
    }

    bool done = false;
    while (!done) {
        bool all_closed = true;
        for (size_t k = 0; k < channel_count; k++) {
            size_t i = indices[k];
            bool closed = false;
            LatValue received;
            if (channel_try_recv(channels[i], &received, &closed)) {
                *selected_out = i;
                *received_out = received;
                done = true;
                break;
            }
            if (!closed) all_closed = false;
        }
        if (done) break;

        if (all_closed && channel_count > 0) {
            if (default_arm >= 0) *selected_out = (size_t)default_arm;
            break;
        }
        if (default_arm >= 0) {
            *selected_out = (size_t)default_arm;
            break;
        }

        bool registered = true;
        for (size_t k = 0; k < channel_count; k++) {
            if (!channel_add_waiter(channels[indices[k]], &waiter)) {
                registered = false;
                break;
            }
        }
        if (!registered) {
            for (size_t k = 0; k < channel_count; k++) channel_remove_waiter(channels[indices[k]], &waiter);
            pthread_cond_destroy(&cond);
            pthread_mutex_destroy(&mutex);
            free(indices);
            return false;
        }

        /* Close the poll/register race before sleeping. A sender either leaves
         * a value for this pass or sets waiter.notified under mutex. */
        for (size_t k = 0; k < channel_count; k++) {
            size_t i = indices[k];
            bool closed = false;
            LatValue received;
            if (channel_try_recv(channels[i], &received, &closed)) {
                *selected_out = i;
                *received_out = received;
                done = true;
                break;
            }
        }

        pthread_mutex_lock(&mutex);
        int wait_result = 0;
        if (!done && !waiter.notified) {
            wait_result =
                timeout_ms >= 0 ? pthread_cond_timedwait(&cond, &mutex, &deadline) : pthread_cond_wait(&cond, &mutex);
        }
        bool was_notified = waiter.notified;
        waiter.notified = false;
        pthread_mutex_unlock(&mutex);
        for (size_t k = 0; k < channel_count; k++) channel_remove_waiter(channels[indices[k]], &waiter);

        if (done) break;
        if (wait_result == ETIMEDOUT && !was_notified) {
            if (timeout_arm >= 0) *selected_out = (size_t)timeout_arm;
            break;
        }
    }

    pthread_cond_destroy(&cond);
    pthread_mutex_destroy(&mutex);
    free(indices);
    return true;
#endif
}
