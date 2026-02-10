#include "ds/vec.h"
#include <stdlib.h>
#include <string.h>

#define INITIAL_CAP 8

LatVec lat_vec_new(size_t elem_size) {
    LatVec v;
    v.data = NULL;
    v.len = 0;
    v.cap = 0;
    v.elem_size = elem_size;
    return v;
}

void lat_vec_free(LatVec *v) {
    free(v->data);
    v->data = NULL;
    v->len = 0;
    v->cap = 0;
}

static void lat_vec_grow(LatVec *v) {
    size_t new_cap = v->cap == 0 ? INITIAL_CAP : v->cap * 2;
    v->data = realloc(v->data, new_cap * v->elem_size);
    v->cap = new_cap;
}

void lat_vec_push(LatVec *v, const void *elem) {
    if (v->len >= v->cap) {
        lat_vec_grow(v);
    }
    memcpy((char *)v->data + v->len * v->elem_size, elem, v->elem_size);
    v->len++;
}

bool lat_vec_pop(LatVec *v, void *out) {
    if (v->len == 0) return false;
    v->len--;
    if (out) {
        memcpy(out, (char *)v->data + v->len * v->elem_size, v->elem_size);
    }
    return true;
}

void *lat_vec_get(const LatVec *v, size_t index) {
    if (index >= v->len) return NULL;
    return (char *)v->data + index * v->elem_size;
}

void lat_vec_set(LatVec *v, size_t index, const void *elem) {
    if (index >= v->len) return;
    memcpy((char *)v->data + index * v->elem_size, elem, v->elem_size);
}

void lat_vec_clear(LatVec *v) {
    v->len = 0;
}

void lat_vec_reserve(LatVec *v, size_t cap) {
    if (cap <= v->cap) return;
    v->data = realloc(v->data, cap * v->elem_size);
    v->cap = cap;
}
