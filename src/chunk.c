#include "chunk.h"
#include "stackopcode.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static size_t chunk_fnv1a(const char *key) {
    size_t hash = 14695981039346656037ULL;
    for (const char *p = key; *p; p++) {
        hash ^= (size_t)(unsigned char)*p;
        hash *= 1099511628211ULL;
    }
    return hash;
}

Chunk *chunk_new(void) {
    Chunk *c = calloc(1, sizeof(Chunk));
    if (!c) return NULL;
    c->code_cap = 256;
    c->code = malloc(c->code_cap);
    c->const_cap = 32;
    c->constants = malloc(c->const_cap * sizeof(LatValue));
    c->const_hashes = calloc(c->const_cap, sizeof(size_t));
    c->lines_cap = 256;
    c->lines = malloc(c->lines_cap * sizeof(int));
    if (!c->code || !c->constants || !c->const_hashes || !c->lines) {
        free(c->code);
        free(c->constants);
        free(c->const_hashes);
        free(c->lines);
        free(c);
        return NULL;
    }
    return c;
}

void chunk_free(Chunk *c) {
    if (!c) return;
    free(c->code);
    for (size_t i = 0; i < c->const_len; i++) {
        /* Recursively free compiled sub-chunks stored as VAL_CLOSURE constants */
        if (c->constants[i].type == VAL_CLOSURE && c->constants[i].as.closure.body == NULL &&
            c->constants[i].as.closure.native_fn != NULL) {
            chunk_free((Chunk *)c->constants[i].as.closure.native_fn);
            c->constants[i].as.closure.native_fn = NULL;
        }
        value_free(&c->constants[i]);
    }
    free(c->constants);
    free(c->const_hashes);
    free(c->lines);
    for (size_t i = 0; i < c->local_name_cap; i++) free(c->local_names[i]);
    free(c->local_names);
    free(c->name);
    if (c->default_values) {
        for (int i = 0; i < c->default_count; i++) value_free(&c->default_values[i]);
        free(c->default_values);
    }
    free(c->param_phases);
    if (c->export_names) {
        for (size_t i = 0; i < c->export_count; i++) free(c->export_names[i]);
        free(c->export_names);
    }
    pic_table_free(&c->pic);
    free(c);
}

void chunk_set_local_name(Chunk *c, size_t slot, const char *name) {
    while (c->local_name_cap <= slot) {
        size_t new_cap = c->local_name_cap ? c->local_name_cap * 2 : 8;
        c->local_names = realloc(c->local_names, new_cap * sizeof(char *));
        for (size_t i = c->local_name_cap; i < new_cap; i++) c->local_names[i] = NULL;
        c->local_name_cap = new_cap;
    }
    free(c->local_names[slot]);
    c->local_names[slot] = strdup(name);
}

size_t chunk_write(Chunk *c, uint8_t byte, int line) {
    if (c->code_len >= c->code_cap) {
        c->code_cap *= 2;
        c->code = realloc(c->code, c->code_cap);
    }
    if (c->lines_len >= c->lines_cap) {
        c->lines_cap *= 2;
        c->lines = realloc(c->lines, c->lines_cap * sizeof(int));
    }
    size_t offset = c->code_len;
    c->code[c->code_len++] = byte;
    c->lines[c->lines_len++] = line;
    return offset;
}

size_t chunk_add_constant(Chunk *c, LatValue val) {
    /* Deduplicate string constants */
    if (val.type == VAL_STR && val.as.str_val) {
        size_t h = chunk_fnv1a(val.as.str_val);
        for (size_t i = 0; i < c->const_len; i++) {
            if (c->const_hashes[i] == h && c->constants[i].type == VAL_STR && c->constants[i].as.str_val &&
                strcmp(c->constants[i].as.str_val, val.as.str_val) == 0) {
                free(val.as.str_val);
                return i;
            }
        }
    }
    /* Deduplicate integer constants */
    if (val.type == VAL_INT) {
        for (size_t i = 0; i < c->const_len; i++) {
            if (c->constants[i].type == VAL_INT && c->constants[i].as.int_val == val.as.int_val) { return i; }
        }
    }
    /* Deduplicate float constants */
    if (val.type == VAL_FLOAT) {
        for (size_t i = 0; i < c->const_len; i++) {
            if (c->constants[i].type == VAL_FLOAT && c->constants[i].as.float_val == val.as.float_val) { return i; }
        }
    }
    if (c->const_len >= c->const_cap) {
        c->const_cap *= 2;
        c->constants = realloc(c->constants, c->const_cap * sizeof(LatValue));
        c->const_hashes = realloc(c->const_hashes, c->const_cap * sizeof(size_t));
    }
    size_t idx = c->const_len++;
    c->constants[idx] = val;
    c->const_hashes[idx] = (val.type == VAL_STR && val.as.str_val) ? chunk_fnv1a(val.as.str_val) : 0;
    return idx;
}

size_t chunk_add_constant_nodupe(Chunk *c, LatValue val) {
    if (c->const_len >= c->const_cap) {
        c->const_cap *= 2;
        c->constants = realloc(c->constants, c->const_cap * sizeof(LatValue));
        c->const_hashes = realloc(c->const_hashes, c->const_cap * sizeof(size_t));
    }
    size_t idx = c->const_len++;
    c->constants[idx] = val;
    c->const_hashes[idx] = (val.type == VAL_STR && val.as.str_val) ? chunk_fnv1a(val.as.str_val) : 0;
    return idx;
}

void chunk_write_u16(Chunk *c, uint16_t val, int line) {
    chunk_write(c, (uint8_t)((val >> 8) & 0xff), line);
    chunk_write(c, (uint8_t)(val & 0xff), line);
}

static size_t simple_instruction(const char *name, size_t offset) {
    fprintf(stderr, "%s\n", name);
    return offset + 1;
}

static size_t byte_instruction(const char *name, const Chunk *c, size_t offset) {
    uint8_t slot = c->code[offset + 1];
    fprintf(stderr, "%-20s %4d\n", name, slot);
    return offset + 2;
}

static size_t constant_instruction(const char *name, const Chunk *c, size_t offset) {
    uint8_t idx = c->code[offset + 1];
    fprintf(stderr, "%-20s %4d '", name, idx);
    if (idx < c->const_len) {
        char *repr = value_repr(&c->constants[idx]);
        fprintf(stderr, "%s", repr);
        free(repr);
    }
    fprintf(stderr, "'\n");
    return offset + 2;
}

static size_t constant_instruction_16(const char *name, const Chunk *c, size_t offset) {
    uint16_t idx = (uint16_t)(c->code[offset + 1] << 8) | c->code[offset + 2];
    fprintf(stderr, "%-20s %4d '", name, idx);
    if (idx < c->const_len) {
        char *repr = value_repr(&c->constants[idx]);
        fprintf(stderr, "%s", repr);
        free(repr);
    }
    fprintf(stderr, "'\n");
    return offset + 3;
}

static size_t jump_instruction(const char *name, int sign, const Chunk *c, size_t offset) {
    uint16_t jump = (uint16_t)(c->code[offset + 1] << 8) | c->code[offset + 2];
    fprintf(stderr, "%-20s %4zu -> %zu\n", name, offset, offset + 3 + sign * jump);
    return offset + 3;
}

static size_t closure_instruction(const Chunk *c, size_t offset) {
    uint8_t fn_idx = c->code[offset + 1];
    uint8_t upvalue_count = c->code[offset + 2];
    fprintf(stderr, "%-20s %4d (upvalues: %d)\n", "OP_CLOSURE", fn_idx, upvalue_count);
    size_t pos = offset + 3;
    for (uint8_t i = 0; i < upvalue_count; i++) {
        uint8_t is_local = c->code[pos++];
        uint8_t index = c->code[pos++];
        fprintf(stderr, "     |                     %s %d\n", is_local ? "local" : "upvalue", index);
    }
    return pos;
}

static size_t invoke_instruction(const Chunk *c, size_t offset) {
    uint8_t method_idx = c->code[offset + 1];
    uint8_t arg_count = c->code[offset + 2];
    fprintf(stderr, "%-20s %4d '", "OP_INVOKE", method_idx);
    if (method_idx < c->const_len) {
        char *repr = value_repr(&c->constants[method_idx]);
        fprintf(stderr, "%s", repr);
        free(repr);
    }
    fprintf(stderr, "' (%d args)\n", arg_count);
    return offset + 3;
}

static size_t build_struct_instruction(const Chunk *c, size_t offset) {
    uint8_t name_idx = c->code[offset + 1];
    uint8_t field_count = c->code[offset + 2];
    fprintf(stderr, "%-20s %4d '", "OP_BUILD_STRUCT", name_idx);
    if (name_idx < c->const_len) {
        char *repr = value_repr(&c->constants[name_idx]);
        fprintf(stderr, "%s", repr);
        free(repr);
    }
    fprintf(stderr, "' (%d fields)\n", field_count);
    return offset + 3;
}

static size_t build_enum_instruction(const Chunk *c, size_t offset) {
    uint8_t enum_idx = c->code[offset + 1];
    uint8_t var_idx = c->code[offset + 2];
    uint8_t payload_count = c->code[offset + 3];
    fprintf(stderr, "%-20s enum=%d var=%d payload=%d\n", "OP_BUILD_ENUM", enum_idx, var_idx, payload_count);
    return offset + 4;
}

size_t chunk_disassemble_instruction(const Chunk *c, size_t offset) {
    fprintf(stderr, "%04zu ", offset);
    if (offset > 0 && c->lines[offset] == c->lines[offset - 1]) fprintf(stderr, "   | ");
    else fprintf(stderr, "%4d ", c->lines[offset]);

    uint8_t op = c->code[offset];
    switch (op) {
        case OP_CONSTANT: return constant_instruction("OP_CONSTANT", c, offset);
        case OP_NIL: return simple_instruction("OP_NIL", offset);
        case OP_TRUE: return simple_instruction("OP_TRUE", offset);
        case OP_FALSE: return simple_instruction("OP_FALSE", offset);
        case OP_UNIT: return simple_instruction("OP_UNIT", offset);
        case OP_POP: return simple_instruction("OP_POP", offset);
        case OP_DUP: return simple_instruction("OP_DUP", offset);
        case OP_SWAP: return simple_instruction("OP_SWAP", offset);
        case OP_ADD: return simple_instruction("OP_ADD", offset);
        case OP_SUB: return simple_instruction("OP_SUB", offset);
        case OP_MUL: return simple_instruction("OP_MUL", offset);
        case OP_DIV: return simple_instruction("OP_DIV", offset);
        case OP_MOD: return simple_instruction("OP_MOD", offset);
        case OP_NEG: return simple_instruction("OP_NEG", offset);
        case OP_NOT: return simple_instruction("OP_NOT", offset);
        case OP_BIT_AND: return simple_instruction("OP_BIT_AND", offset);
        case OP_BIT_OR: return simple_instruction("OP_BIT_OR", offset);
        case OP_BIT_XOR: return simple_instruction("OP_BIT_XOR", offset);
        case OP_BIT_NOT: return simple_instruction("OP_BIT_NOT", offset);
        case OP_LSHIFT: return simple_instruction("OP_LSHIFT", offset);
        case OP_RSHIFT: return simple_instruction("OP_RSHIFT", offset);
        case OP_EQ: return simple_instruction("OP_EQ", offset);
        case OP_NEQ: return simple_instruction("OP_NEQ", offset);
        case OP_LT: return simple_instruction("OP_LT", offset);
        case OP_GT: return simple_instruction("OP_GT", offset);
        case OP_LTEQ: return simple_instruction("OP_LTEQ", offset);
        case OP_GTEQ: return simple_instruction("OP_GTEQ", offset);
        case OP_CONCAT: return simple_instruction("OP_CONCAT", offset);
        case OP_GET_LOCAL: return byte_instruction("OP_GET_LOCAL", c, offset);
        case OP_SET_LOCAL: return byte_instruction("OP_SET_LOCAL", c, offset);
        case OP_GET_GLOBAL: return constant_instruction("OP_GET_GLOBAL", c, offset);
        case OP_SET_GLOBAL: return constant_instruction("OP_SET_GLOBAL", c, offset);
        case OP_DEFINE_GLOBAL: return constant_instruction("OP_DEFINE_GLOBAL", c, offset);
        case OP_GET_UPVALUE: return byte_instruction("OP_GET_UPVALUE", c, offset);
        case OP_SET_UPVALUE: return byte_instruction("OP_SET_UPVALUE", c, offset);
        case OP_CLOSE_UPVALUE: return simple_instruction("OP_CLOSE_UPVALUE", offset);
        case OP_JUMP: return jump_instruction("OP_JUMP", 1, c, offset);
        case OP_JUMP_IF_FALSE: return jump_instruction("OP_JUMP_IF_FALSE", 1, c, offset);
        case OP_JUMP_IF_TRUE: return jump_instruction("OP_JUMP_IF_TRUE", 1, c, offset);
        case OP_JUMP_IF_NOT_NIL: return jump_instruction("OP_JUMP_IF_NOT_NIL", 1, c, offset);
        case OP_LOOP: return jump_instruction("OP_LOOP", -1, c, offset);
        case OP_CALL: return byte_instruction("OP_CALL", c, offset);
        case OP_CLOSURE: return closure_instruction(c, offset);
        case OP_RETURN: return simple_instruction("OP_RETURN", offset);
        case OP_ITER_INIT: return simple_instruction("OP_ITER_INIT", offset);
        case OP_ITER_NEXT: return jump_instruction("OP_ITER_NEXT", 1, c, offset);
        case OP_BUILD_ARRAY: return byte_instruction("OP_BUILD_ARRAY", c, offset);
        case OP_ARRAY_FLATTEN: return simple_instruction("OP_ARRAY_FLATTEN", offset);
        case OP_BUILD_MAP: return byte_instruction("OP_BUILD_MAP", c, offset);
        case OP_BUILD_TUPLE: return byte_instruction("OP_BUILD_TUPLE", c, offset);
        case OP_BUILD_STRUCT: return build_struct_instruction(c, offset);
        case OP_BUILD_RANGE: return simple_instruction("OP_BUILD_RANGE", offset);
        case OP_BUILD_ENUM: return build_enum_instruction(c, offset);
        case OP_INDEX: return simple_instruction("OP_INDEX", offset);
        case OP_SET_INDEX: return simple_instruction("OP_SET_INDEX", offset);
        case OP_GET_FIELD: return constant_instruction("OP_GET_FIELD", c, offset);
        case OP_SET_FIELD: return constant_instruction("OP_SET_FIELD", c, offset);
        case OP_INVOKE: return invoke_instruction(c, offset);
        case OP_INVOKE_LOCAL: {
            uint8_t slot = c->code[offset + 1];
            uint8_t method_idx = c->code[offset + 2];
            uint8_t argc = c->code[offset + 3];
            fprintf(stderr, "%-20s slot=%d '", "OP_INVOKE_LOCAL", slot);
            if (method_idx < c->const_len) {
                char *repr = value_repr(&c->constants[method_idx]);
                fprintf(stderr, "%s", repr);
                free(repr);
            }
            fprintf(stderr, "' (%d args)\n", argc);
            return offset + 4;
        }
        case OP_INVOKE_GLOBAL: {
            uint8_t name_idx = c->code[offset + 1];
            uint8_t method_idx = c->code[offset + 2];
            uint8_t argc = c->code[offset + 3];
            fprintf(stderr, "%-20s '", "OP_INVOKE_GLOBAL");
            if (name_idx < c->const_len) {
                char *repr = value_repr(&c->constants[name_idx]);
                fprintf(stderr, "%s", repr);
                free(repr);
            }
            fprintf(stderr, "'.'");
            if (method_idx < c->const_len) {
                char *repr = value_repr(&c->constants[method_idx]);
                fprintf(stderr, "%s", repr);
                free(repr);
            }
            fprintf(stderr, "' (%d args)\n", argc);
            return offset + 4;
        }
        case OP_INVOKE_LOCAL_16: {
            uint8_t slot = c->code[offset + 1];
            uint16_t method_idx = (uint16_t)(c->code[offset + 2] << 8) | c->code[offset + 3];
            uint8_t argc = c->code[offset + 4];
            fprintf(stderr, "%-20s slot=%d '", "OP_INVOKE_LOCAL_16", slot);
            if (method_idx < c->const_len) {
                char *repr = value_repr(&c->constants[method_idx]);
                fprintf(stderr, "%s", repr);
                free(repr);
            }
            fprintf(stderr, "' (%d args)\n", argc);
            return offset + 5;
        }
        case OP_INVOKE_GLOBAL_16: {
            uint16_t name_idx = (uint16_t)(c->code[offset + 1] << 8) | c->code[offset + 2];
            uint16_t method_idx = (uint16_t)(c->code[offset + 3] << 8) | c->code[offset + 4];
            uint8_t argc = c->code[offset + 5];
            fprintf(stderr, "%-20s '", "OP_INVOKE_GLOBAL_16");
            if (name_idx < c->const_len) {
                char *repr = value_repr(&c->constants[name_idx]);
                fprintf(stderr, "%s", repr);
                free(repr);
            }
            fprintf(stderr, "'.'");
            if (method_idx < c->const_len) {
                char *repr = value_repr(&c->constants[method_idx]);
                fprintf(stderr, "%s", repr);
                free(repr);
            }
            fprintf(stderr, "' (%d args)\n", argc);
            return offset + 6;
        }
        case OP_SET_INDEX_LOCAL: return byte_instruction("OP_SET_INDEX_LOCAL", c, offset);
        case OP_PUSH_EXCEPTION_HANDLER: return jump_instruction("OP_PUSH_EXCEPTION_HANDLER", 1, c, offset);
        case OP_POP_EXCEPTION_HANDLER: return simple_instruction("OP_POP_EXCEPTION_HANDLER", offset);
        case OP_THROW: return simple_instruction("OP_THROW", offset);
        case OP_TRY_UNWRAP: return simple_instruction("OP_TRY_UNWRAP", offset);
        case OP_DEFER_PUSH: return jump_instruction("OP_DEFER_PUSH", 1, c, offset);
        case OP_DEFER_RUN: return simple_instruction("OP_DEFER_RUN", offset);
        case OP_FREEZE: return simple_instruction("OP_FREEZE", offset);
        case OP_THAW: return simple_instruction("OP_THAW", offset);
        case OP_CLONE: return simple_instruction("OP_CLONE", offset);
        case OP_MARK_FLUID: return simple_instruction("OP_MARK_FLUID", offset);
        case OP_PRINT: return byte_instruction("OP_PRINT", c, offset);
        case OP_IMPORT: return byte_instruction("OP_IMPORT", c, offset);
        case OP_SCOPE: {
            uint8_t spawn_count = c->code[offset + 1];
            uint8_t sync_idx = c->code[offset + 2];
            fprintf(stderr, "%-20s spawns=%d sync=%d", "OP_SCOPE", spawn_count, sync_idx);
            for (uint8_t i = 0; i < spawn_count; i++) fprintf(stderr, " spawn[%d]=%d", i, c->code[offset + 3 + i]);
            fprintf(stderr, "\n");
            return offset + 3 + spawn_count;
        }
        case OP_SELECT: {
            uint8_t arm_count = c->code[offset + 1];
            fprintf(stderr, "%-20s arms=%d\n", "OP_SELECT", arm_count);
            size_t pos = offset + 2;
            for (uint8_t i = 0; i < arm_count; i++) {
                uint8_t flags = c->code[pos++];
                uint8_t chan_idx = c->code[pos++];
                uint8_t body_idx = c->code[pos++];
                uint8_t bind_idx = c->code[pos++];
                fprintf(stderr, "     |                     arm %d: flags=%02x chan=%d body=%d bind=%d\n", i, flags,
                        chan_idx, body_idx, bind_idx);
            }
            return pos;
        }
        case OP_INC_LOCAL: return byte_instruction("OP_INC_LOCAL", c, offset);
        case OP_DEC_LOCAL: return byte_instruction("OP_DEC_LOCAL", c, offset);
        case OP_ADD_INT: return simple_instruction("OP_ADD_INT", offset);
        case OP_SUB_INT: return simple_instruction("OP_SUB_INT", offset);
        case OP_MUL_INT: return simple_instruction("OP_MUL_INT", offset);
        case OP_LT_INT: return simple_instruction("OP_LT_INT", offset);
        case OP_LTEQ_INT: return simple_instruction("OP_LTEQ_INT", offset);
        case OP_LOAD_INT8: return byte_instruction("OP_LOAD_INT8", c, offset);
        case OP_CONSTANT_16: return constant_instruction_16("OP_CONSTANT_16", c, offset);
        case OP_GET_GLOBAL_16: return constant_instruction_16("OP_GET_GLOBAL_16", c, offset);
        case OP_SET_GLOBAL_16: return constant_instruction_16("OP_SET_GLOBAL_16", c, offset);
        case OP_DEFINE_GLOBAL_16: return constant_instruction_16("OP_DEFINE_GLOBAL_16", c, offset);
        case OP_CLOSURE_16: {
            uint16_t idx = (uint16_t)(c->code[offset + 1] << 8) | c->code[offset + 2];
            uint8_t uvc = c->code[offset + 3];
            fprintf(stderr, "%-20s %4d (upvalues: %d)\n", "OP_CLOSURE_16", idx, uvc);
            return offset + 4 + uvc * 2;
        }
        case OP_RESET_EPHEMERAL: return simple_instruction("OP_RESET_EPHEMERAL", offset);
        case OP_SET_LOCAL_POP: return byte_instruction("OP_SET_LOCAL_POP", c, offset);
        case OP_SET_SLICE: return simple_instruction("OP_SET_SLICE", offset);
        case OP_SET_SLICE_LOCAL: return byte_instruction("OP_SET_SLICE_LOCAL", c, offset);
        case OP_HALT: return simple_instruction("OP_HALT", offset);
        default: fprintf(stderr, "Unknown opcode %d\n", op); return offset + 1;
    }
}

void chunk_disassemble(const Chunk *c, const char *name) {
    fprintf(stderr, "== %s ==\n", name);
    for (size_t offset = 0; offset < c->code_len;) offset = chunk_disassemble_instruction(c, offset);
}
