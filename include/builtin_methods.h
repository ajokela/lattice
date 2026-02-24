#ifndef BUILTIN_METHODS_H
#define BUILTIN_METHODS_H

#include "value.h"

/*
 * Shared builtin method implementations for array, string, map, buffer, set,
 * enum, tuple, and range types.
 *
 * These functions implement the CORE LOGIC of each method, independent of
 * the VM's argument-passing convention (stack-based vs register-based).
 *
 * Signature convention:
 *   - obj:       pointer to the receiver value (may be mutated for in-place ops)
 *   - args:      array of argument values (already extracted by the caller)
 *   - arg_count: number of arguments
 *   - error:     out-param; set to a heap-allocated error string on failure
 *   - return:    the result value
 *
 * The caller is responsible for cloning args if needed before passing them in.
 * Args are NOT freed by these functions.
 */

/* ── Callback type for closure-requiring methods ──
 *
 * Each VM provides its own implementation that knows how to invoke a closure
 * in its calling convention (stack-based for StackVM, register windows for RegVM).
 *
 *   closure:    opaque pointer to the closure value (VM-specific)
 *   args:       array of argument values to pass to the closure
 *   arg_count:  number of arguments
 *   ctx:        opaque VM context (cast to StackVM* or RegVM* by the callback)
 *   return:     the closure's return value (caller owns it)
 */
typedef LatValue (*BuiltinCallback)(void *closure, LatValue *args, int arg_count, void *ctx);

/* ── Array methods (no closures) ── */

LatValue builtin_array_contains(LatValue *obj, LatValue *args, int arg_count, char **error);
LatValue builtin_array_enumerate(LatValue *obj, LatValue *args, int arg_count, char **error);
LatValue builtin_array_reverse(LatValue *obj, LatValue *args, int arg_count, char **error);
LatValue builtin_array_join(LatValue *obj, LatValue *args, int arg_count, char **error);
LatValue builtin_array_unique(LatValue *obj, LatValue *args, int arg_count, char **error);
LatValue builtin_array_index_of(LatValue *obj, LatValue *args, int arg_count, char **error);
LatValue builtin_array_zip(LatValue *obj, LatValue *args, int arg_count, char **error);
LatValue builtin_array_sum(LatValue *obj, LatValue *args, int arg_count, char **error);
LatValue builtin_array_min(LatValue *obj, LatValue *args, int arg_count, char **error);
LatValue builtin_array_max(LatValue *obj, LatValue *args, int arg_count, char **error);
LatValue builtin_array_first(LatValue *obj, LatValue *args, int arg_count, char **error);
LatValue builtin_array_last(LatValue *obj, LatValue *args, int arg_count, char **error);
LatValue builtin_array_take(LatValue *obj, LatValue *args, int arg_count, char **error);
LatValue builtin_array_drop(LatValue *obj, LatValue *args, int arg_count, char **error);
LatValue builtin_array_chunk(LatValue *obj, LatValue *args, int arg_count, char **error);
LatValue builtin_array_flatten(LatValue *obj, LatValue *args, int arg_count, char **error);

/* ── Array methods (with closures) ── */

LatValue builtin_array_map(LatValue *obj, void *closure, BuiltinCallback cb, void *ctx, char **error);
LatValue builtin_array_filter(LatValue *obj, void *closure, BuiltinCallback cb, void *ctx, char **error);
LatValue builtin_array_reduce(LatValue *obj, LatValue *init, bool has_init,
                              void *closure, BuiltinCallback cb, void *ctx, char **error);
LatValue builtin_array_each(LatValue *obj, void *closure, BuiltinCallback cb, void *ctx, char **error);
LatValue builtin_array_find(LatValue *obj, void *closure, BuiltinCallback cb, void *ctx, char **error);
LatValue builtin_array_any(LatValue *obj, void *closure, BuiltinCallback cb, void *ctx, char **error);
LatValue builtin_array_all(LatValue *obj, void *closure, BuiltinCallback cb, void *ctx, char **error);
LatValue builtin_array_flat_map(LatValue *obj, void *closure, BuiltinCallback cb, void *ctx, char **error);
LatValue builtin_array_sort_by(LatValue *obj, void *closure, BuiltinCallback cb, void *ctx, char **error);
LatValue builtin_array_group_by(LatValue *obj, void *closure, BuiltinCallback cb, void *ctx, char **error);

/* ── String methods ── */

LatValue builtin_string_split(LatValue *obj, LatValue *args, int arg_count, char **error);
LatValue builtin_string_trim(LatValue *obj, LatValue *args, int arg_count, char **error);
LatValue builtin_string_trim_start(LatValue *obj, LatValue *args, int arg_count, char **error);
LatValue builtin_string_trim_end(LatValue *obj, LatValue *args, int arg_count, char **error);
LatValue builtin_string_to_upper(LatValue *obj, LatValue *args, int arg_count, char **error);
LatValue builtin_string_to_lower(LatValue *obj, LatValue *args, int arg_count, char **error);
LatValue builtin_string_starts_with(LatValue *obj, LatValue *args, int arg_count, char **error);
LatValue builtin_string_ends_with(LatValue *obj, LatValue *args, int arg_count, char **error);
LatValue builtin_string_replace(LatValue *obj, LatValue *args, int arg_count, char **error);
LatValue builtin_string_contains(LatValue *obj, LatValue *args, int arg_count, char **error);
LatValue builtin_string_chars(LatValue *obj, LatValue *args, int arg_count, char **error);
LatValue builtin_string_bytes(LatValue *obj, LatValue *args, int arg_count, char **error);
LatValue builtin_string_reverse(LatValue *obj, LatValue *args, int arg_count, char **error);
LatValue builtin_string_repeat(LatValue *obj, LatValue *args, int arg_count, char **error);
LatValue builtin_string_pad_left(LatValue *obj, LatValue *args, int arg_count, char **error);
LatValue builtin_string_pad_right(LatValue *obj, LatValue *args, int arg_count, char **error);
LatValue builtin_string_count(LatValue *obj, LatValue *args, int arg_count, char **error);
LatValue builtin_string_is_empty(LatValue *obj, LatValue *args, int arg_count, char **error);
LatValue builtin_string_index_of(LatValue *obj, LatValue *args, int arg_count, char **error);
LatValue builtin_string_substring(LatValue *obj, LatValue *args, int arg_count, char **error);

/* ── Map methods (no closures) ── */

LatValue builtin_map_keys(LatValue *obj, LatValue *args, int arg_count, char **error);
LatValue builtin_map_values(LatValue *obj, LatValue *args, int arg_count, char **error);
LatValue builtin_map_entries(LatValue *obj, LatValue *args, int arg_count, char **error);
LatValue builtin_map_get(LatValue *obj, LatValue *args, int arg_count, char **error);
LatValue builtin_map_has(LatValue *obj, LatValue *args, int arg_count, char **error);
LatValue builtin_map_remove(LatValue *obj, LatValue *args, int arg_count, char **error);
LatValue builtin_map_merge(LatValue *obj, LatValue *args, int arg_count, char **error);

/* ── Buffer methods ── */

LatValue builtin_buffer_push(LatValue *obj, LatValue *args, int arg_count, char **error);
LatValue builtin_buffer_push_u16(LatValue *obj, LatValue *args, int arg_count, char **error);
LatValue builtin_buffer_push_u32(LatValue *obj, LatValue *args, int arg_count, char **error);
LatValue builtin_buffer_read_u8(LatValue *obj, LatValue *args, int arg_count, char **error);
LatValue builtin_buffer_write_u8(LatValue *obj, LatValue *args, int arg_count, char **error);
LatValue builtin_buffer_read_u16(LatValue *obj, LatValue *args, int arg_count, char **error);
LatValue builtin_buffer_write_u16(LatValue *obj, LatValue *args, int arg_count, char **error);
LatValue builtin_buffer_read_u32(LatValue *obj, LatValue *args, int arg_count, char **error);
LatValue builtin_buffer_write_u32(LatValue *obj, LatValue *args, int arg_count, char **error);
LatValue builtin_buffer_read_i8(LatValue *obj, LatValue *args, int arg_count, char **error);
LatValue builtin_buffer_read_i16(LatValue *obj, LatValue *args, int arg_count, char **error);
LatValue builtin_buffer_read_i32(LatValue *obj, LatValue *args, int arg_count, char **error);
LatValue builtin_buffer_read_f32(LatValue *obj, LatValue *args, int arg_count, char **error);
LatValue builtin_buffer_read_f64(LatValue *obj, LatValue *args, int arg_count, char **error);
LatValue builtin_buffer_slice(LatValue *obj, LatValue *args, int arg_count, char **error);
LatValue builtin_buffer_clear(LatValue *obj, LatValue *args, int arg_count, char **error);
LatValue builtin_buffer_fill(LatValue *obj, LatValue *args, int arg_count, char **error);
LatValue builtin_buffer_resize(LatValue *obj, LatValue *args, int arg_count, char **error);
LatValue builtin_buffer_to_string(LatValue *obj, LatValue *args, int arg_count, char **error);
LatValue builtin_buffer_to_array(LatValue *obj, LatValue *args, int arg_count, char **error);
LatValue builtin_buffer_to_hex(LatValue *obj, LatValue *args, int arg_count, char **error);

/* ── Set methods ── */

LatValue builtin_set_has(LatValue *obj, LatValue *args, int arg_count, char **error);
LatValue builtin_set_add(LatValue *obj, LatValue *args, int arg_count, char **error);
LatValue builtin_set_remove(LatValue *obj, LatValue *args, int arg_count, char **error);
LatValue builtin_set_to_array(LatValue *obj, LatValue *args, int arg_count, char **error);
LatValue builtin_set_union(LatValue *obj, LatValue *args, int arg_count, char **error);
LatValue builtin_set_intersection(LatValue *obj, LatValue *args, int arg_count, char **error);
LatValue builtin_set_difference(LatValue *obj, LatValue *args, int arg_count, char **error);
LatValue builtin_set_is_subset(LatValue *obj, LatValue *args, int arg_count, char **error);
LatValue builtin_set_is_superset(LatValue *obj, LatValue *args, int arg_count, char **error);

/* ── Enum methods ── */

LatValue builtin_enum_tag(LatValue *obj, LatValue *args, int arg_count, char **error);
LatValue builtin_enum_enum_name(LatValue *obj, LatValue *args, int arg_count, char **error);
LatValue builtin_enum_payload(LatValue *obj, LatValue *args, int arg_count, char **error);
LatValue builtin_enum_is_variant(LatValue *obj, LatValue *args, int arg_count, char **error);

#endif /* BUILTIN_METHODS_H */
