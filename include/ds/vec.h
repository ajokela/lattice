#ifndef LAT_VEC_H
#define LAT_VEC_H

#include <stddef.h>
#include <stdbool.h>

/* Generic dynamic array */
typedef struct {
    void  *data;
    size_t len;
    size_t cap;
    size_t elem_size;
} LatVec;

/* Create a new empty vec with given element size */
LatVec lat_vec_new(size_t elem_size);

/* Free all memory */
void lat_vec_free(LatVec *v);

/* Push an element (copies elem_size bytes from ptr) */
void lat_vec_push(LatVec *v, const void *elem);

/* Pop the last element, copies into out if non-NULL. Returns false if empty. */
bool lat_vec_pop(LatVec *v, void *out);

/* Get pointer to element at index */
void *lat_vec_get(const LatVec *v, size_t index);

/* Set element at index (copies elem_size bytes from ptr) */
void lat_vec_set(LatVec *v, size_t index, const void *elem);

/* Clear all elements (keeps capacity) */
void lat_vec_clear(LatVec *v);

/* Reserve capacity for at least `cap` elements total */
void lat_vec_reserve(LatVec *v, size_t cap);

#endif /* LAT_VEC_H */
