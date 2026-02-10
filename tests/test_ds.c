#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ds/str.h"
#include "ds/vec.h"
#include "ds/hashmap.h"

/* Import test macros from test_main.c */
extern void register_test(const char *name, void (*fn)(void));
extern int test_current_failed;

#define ASSERT(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "  FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        test_current_failed = 1; \
        return; \
    } \
} while(0)

#define ASSERT_EQ_INT(a, b) do { \
    long long _a = (long long)(a), _b = (long long)(b); \
    if (_a != _b) { \
        fprintf(stderr, "  FAIL: %s:%d: %lld != %lld\n", __FILE__, __LINE__, _a, _b); \
        test_current_failed = 1; \
        return; \
    } \
} while(0)

#define ASSERT_EQ_STR(a, b) do { \
    const char *_a = (a), *_b = (b); \
    if (strcmp(_a, _b) != 0) { \
        fprintf(stderr, "  FAIL: %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, _a, _b); \
        test_current_failed = 1; \
        return; \
    } \
} while(0)

#define TEST(name) \
    static void name(void); \
    static void name##_register(void) __attribute__((constructor)); \
    static void name##_register(void) { register_test(#name, name); } \
    static void name(void)

/* ── LatStr tests ── */

TEST(str_new_is_empty) {
    LatStr s = lat_str_new();
    ASSERT_EQ_INT(s.len, 0);
    ASSERT_EQ_STR(lat_str_cstr(&s), "");
    lat_str_free(&s);
}

TEST(str_from_cstr) {
    LatStr s = lat_str_from("hello");
    ASSERT_EQ_INT(s.len, 5);
    ASSERT_EQ_STR(lat_str_cstr(&s), "hello");
    lat_str_free(&s);
}

TEST(str_push_char) {
    LatStr s = lat_str_new();
    lat_str_push(&s, 'a');
    lat_str_push(&s, 'b');
    lat_str_push(&s, 'c');
    ASSERT_EQ_INT(s.len, 3);
    ASSERT_EQ_STR(lat_str_cstr(&s), "abc");
    lat_str_free(&s);
}

TEST(str_append) {
    LatStr s = lat_str_from("hello");
    lat_str_append(&s, " world");
    ASSERT_EQ_INT(s.len, 11);
    ASSERT_EQ_STR(lat_str_cstr(&s), "hello world");
    lat_str_free(&s);
}

TEST(str_eq) {
    LatStr a = lat_str_from("test");
    LatStr b = lat_str_from("test");
    LatStr c = lat_str_from("other");
    ASSERT(lat_str_eq(&a, &b));
    ASSERT(!lat_str_eq(&a, &c));
    lat_str_free(&a);
    lat_str_free(&b);
    lat_str_free(&c);
}

TEST(str_dup) {
    LatStr a = lat_str_from("original");
    LatStr b = lat_str_dup(&a);
    ASSERT(lat_str_eq(&a, &b));
    lat_str_push(&b, '!');
    ASSERT(!lat_str_eq(&a, &b));
    lat_str_free(&a);
    lat_str_free(&b);
}

TEST(str_appendf) {
    LatStr s = lat_str_new();
    lat_str_appendf(&s, "%d + %d = %d", 1, 2, 3);
    ASSERT_EQ_STR(lat_str_cstr(&s), "1 + 2 = 3");
    lat_str_free(&s);
}

TEST(str_clear) {
    LatStr s = lat_str_from("data");
    lat_str_clear(&s);
    ASSERT_EQ_INT(s.len, 0);
    ASSERT_EQ_STR(lat_str_cstr(&s), "");
    lat_str_free(&s);
}

/* ── LatVec tests ── */

TEST(vec_new_is_empty) {
    LatVec v = lat_vec_new(sizeof(int));
    ASSERT_EQ_INT(v.len, 0);
    lat_vec_free(&v);
}

TEST(vec_push_and_get) {
    LatVec v = lat_vec_new(sizeof(int));
    int a = 10, b = 20, c = 30;
    lat_vec_push(&v, &a);
    lat_vec_push(&v, &b);
    lat_vec_push(&v, &c);
    ASSERT_EQ_INT(v.len, 3);
    ASSERT_EQ_INT(*(int *)lat_vec_get(&v, 0), 10);
    ASSERT_EQ_INT(*(int *)lat_vec_get(&v, 1), 20);
    ASSERT_EQ_INT(*(int *)lat_vec_get(&v, 2), 30);
    lat_vec_free(&v);
}

TEST(vec_pop) {
    LatVec v = lat_vec_new(sizeof(int));
    int a = 42;
    lat_vec_push(&v, &a);
    int out = 0;
    ASSERT(lat_vec_pop(&v, &out));
    ASSERT_EQ_INT(out, 42);
    ASSERT_EQ_INT(v.len, 0);
    ASSERT(!lat_vec_pop(&v, &out));
    lat_vec_free(&v);
}

TEST(vec_set) {
    LatVec v = lat_vec_new(sizeof(int));
    int a = 1, b = 2;
    lat_vec_push(&v, &a);
    lat_vec_set(&v, 0, &b);
    ASSERT_EQ_INT(*(int *)lat_vec_get(&v, 0), 2);
    lat_vec_free(&v);
}

TEST(vec_grow) {
    LatVec v = lat_vec_new(sizeof(int));
    for (int i = 0; i < 100; i++) {
        lat_vec_push(&v, &i);
    }
    ASSERT_EQ_INT(v.len, 100);
    ASSERT_EQ_INT(*(int *)lat_vec_get(&v, 50), 50);
    ASSERT_EQ_INT(*(int *)lat_vec_get(&v, 99), 99);
    lat_vec_free(&v);
}

/* ── LatMap tests ── */

TEST(map_new_is_empty) {
    LatMap m = lat_map_new(sizeof(int));
    ASSERT_EQ_INT(lat_map_len(&m), 0);
    lat_map_free(&m);
}

TEST(map_set_and_get) {
    LatMap m = lat_map_new(sizeof(int));
    int v1 = 10, v2 = 20;
    lat_map_set(&m, "a", &v1);
    lat_map_set(&m, "b", &v2);
    ASSERT_EQ_INT(lat_map_len(&m), 2);
    ASSERT_EQ_INT(*(int *)lat_map_get(&m, "a"), 10);
    ASSERT_EQ_INT(*(int *)lat_map_get(&m, "b"), 20);
    ASSERT(lat_map_get(&m, "c") == NULL);
    lat_map_free(&m);
}

TEST(map_update) {
    LatMap m = lat_map_new(sizeof(int));
    int v1 = 10, v2 = 99;
    lat_map_set(&m, "key", &v1);
    ASSERT_EQ_INT(*(int *)lat_map_get(&m, "key"), 10);
    lat_map_set(&m, "key", &v2);
    ASSERT_EQ_INT(*(int *)lat_map_get(&m, "key"), 99);
    ASSERT_EQ_INT(lat_map_len(&m), 1);
    lat_map_free(&m);
}

TEST(map_remove) {
    LatMap m = lat_map_new(sizeof(int));
    int v = 42;
    lat_map_set(&m, "x", &v);
    ASSERT(lat_map_contains(&m, "x"));
    ASSERT(lat_map_remove(&m, "x"));
    ASSERT(!lat_map_contains(&m, "x"));
    ASSERT_EQ_INT(lat_map_len(&m), 0);
    ASSERT(!lat_map_remove(&m, "x")); /* already removed */
    lat_map_free(&m);
}

TEST(map_many_entries) {
    LatMap m = lat_map_new(sizeof(int));
    char buf[16];
    for (int i = 0; i < 100; i++) {
        snprintf(buf, sizeof(buf), "key_%d", i);
        lat_map_set(&m, buf, &i);
    }
    ASSERT_EQ_INT(lat_map_len(&m), 100);
    for (int i = 0; i < 100; i++) {
        snprintf(buf, sizeof(buf), "key_%d", i);
        int *val = lat_map_get(&m, buf);
        ASSERT(val != NULL);
        ASSERT_EQ_INT(*val, i);
    }
    lat_map_free(&m);
}

static void sum_values(const char *key, void *value, void *ctx) {
    (void)key;
    int *sum = (int *)ctx;
    *sum += *(int *)value;
}

TEST(map_iter) {
    LatMap m = lat_map_new(sizeof(int));
    int a = 1, b = 2, c = 3;
    lat_map_set(&m, "a", &a);
    lat_map_set(&m, "b", &b);
    lat_map_set(&m, "c", &c);
    int sum = 0;
    lat_map_iter(&m, sum_values, &sum);
    ASSERT_EQ_INT(sum, 6);
    ASSERT_EQ_INT(lat_map_len(&m), 3);
    lat_map_free(&m);
}
