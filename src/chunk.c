#include "chunk.h"
#include "stackopcode.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

/* Marker sentinels for native/extension closures (matches stackvm.c/regvm.c) */
#define VM_NATIVE_MARKER ((struct Expr **)(uintptr_t)0x1)
#define VM_EXT_MARKER    ((struct Expr **)(uintptr_t)0x2)

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
        LatValue *v = &c->constants[i];
        /* Recursively free compiled sub-chunks stored as VAL_CLOSURE constants.
         * Bytecode closure prototypes own param_names — runtime clones borrow
         * (param_names=NULL), so we free them here explicitly. */
        if (v->type == VAL_CLOSURE && v->as.closure.body == NULL && v->as.closure.native_fn != NULL &&
            v->as.closure.default_values != VM_NATIVE_MARKER && v->as.closure.default_values != VM_EXT_MARKER) {
            /* Free prototype-owned param_names */
            if (v->as.closure.param_names) {
                for (size_t pi = 0; pi < v->as.closure.param_count; pi++) free(v->as.closure.param_names[pi]);
                free(v->as.closure.param_names);
                v->as.closure.param_names = NULL;
            }
            chunk_free((Chunk *)v->as.closure.native_fn);
            v->as.closure.native_fn = NULL;
        } else {
            value_free(v);
        }
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
        case OP_INDEX_LOCAL: return byte_instruction("OP_INDEX_LOCAL", c, offset);
        case OP_GET_FIELD_LOCAL: {
            uint8_t slot = c->code[offset + 1];
            uint8_t field_idx = c->code[offset + 2];
            fprintf(stderr, "%-20s slot=%d '", "OP_GET_FIELD_LOCAL", slot);
            if (field_idx < c->const_len) {
                char *repr = value_repr(&c->constants[field_idx]);
                fprintf(stderr, "%s", repr);
                free(repr);
            }
            fprintf(stderr, "'\n");
            return offset + 3;
        }
        case OP_HALT: return simple_instruction("OP_HALT", offset);
        case OP_INDEX_GLOBAL: return constant_instruction_16("OP_INDEX_GLOBAL", c, offset);
        case OP_SET_INDEX_GLOBAL: return constant_instruction_16("OP_SET_INDEX_GLOBAL", c, offset);
        default: fprintf(stderr, "Unknown opcode %d\n", op); return offset + 1;
    }
}

void chunk_disassemble(const Chunk *c, const char *name) {
    fprintf(stderr, "== %s ==\n", name);
    for (size_t offset = 0; offset < c->code_len;) offset = chunk_disassemble_instruction(c, offset);
}

/* ═══════════════════════════════════════════════════════
 * Bytecode verification
 *
 * A .latc file is untrusted input. Before a deserialized chunk is handed to
 * the VM (whose dispatch loop trusts every operand and has no ip-bounds
 * check), it must be verified: known opcodes only, no operand runs past the
 * end of the code, every constant/local/jump operand is in range and of the
 * expected type, jumps land on instruction boundaries, and the code ends in a
 * control-terminating instruction. Verification recurses into sub-chunks
 * (function/scope/select bodies stored as VAL_CLOSURE constants).
 * ═══════════════════════════════════════════════════════ */

#define CHUNK_VERIFY_MAX_DEPTH 200

static char *verify_failf(const char *fmt, ...) {
    char buf[192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return strdup(buf);
}

/* Length (opcode + operands) of the instruction at `off`, after confirming all
 * of its bytes — including count-driven variable parts — lie within code_len.
 * Returns 0 and sets *err on any malformation. */
static size_t verify_instr_length(const Chunk *c, size_t off, char **err) {
    uint8_t op = c->code[off];
    size_t remaining = c->code_len - off; /* >= 1, since off < code_len */
#define NEED(n)                                                                             \
    do {                                                                                    \
        if (remaining < (size_t)(n)) {                                                      \
            *err = verify_failf("truncated operands for opcode %u at offset %zu", op, off); \
            return 0;                                                                       \
        }                                                                                   \
    } while (0)
    switch (op) {
        /* 1-byte */
        case OP_NIL:
        case OP_TRUE:
        case OP_FALSE:
        case OP_UNIT:
        case OP_POP:
        case OP_DUP:
        case OP_SWAP:
        case OP_ADD:
        case OP_SUB:
        case OP_MUL:
        case OP_DIV:
        case OP_MOD:
        case OP_NEG:
        case OP_NOT:
        case OP_BIT_AND:
        case OP_BIT_OR:
        case OP_BIT_XOR:
        case OP_BIT_NOT:
        case OP_LSHIFT:
        case OP_RSHIFT:
        case OP_EQ:
        case OP_NEQ:
        case OP_LT:
        case OP_GT:
        case OP_LTEQ:
        case OP_GTEQ:
        case OP_CONCAT:
        case OP_CLOSE_UPVALUE:
        case OP_RETURN:
        case OP_ITER_INIT:
        case OP_ARRAY_FLATTEN:
        case OP_BUILD_RANGE:
        case OP_INDEX:
        case OP_SET_INDEX:
        case OP_POP_EXCEPTION_HANDLER:
        case OP_THROW:
        case OP_TRY_UNWRAP:
        case OP_FREEZE:
        case OP_THAW:
        case OP_CLONE:
        case OP_MARK_FLUID:
        case OP_SUBLIMATE:
        case OP_IS_CRYSTAL:
        case OP_IS_FLUID:
        case OP_ADD_INT:
        case OP_SUB_INT:
        case OP_MUL_INT:
        case OP_LT_INT:
        case OP_LTEQ_INT:
        case OP_RESET_EPHEMERAL:
        case OP_SET_SLICE:
        case OP_HALT: return 1;
        /* 2-byte */
        case OP_CONSTANT:
        case OP_GET_LOCAL:
        case OP_SET_LOCAL:
        case OP_SET_LOCAL_POP:
        case OP_GET_GLOBAL:
        case OP_SET_GLOBAL:
        case OP_DEFINE_GLOBAL:
        case OP_GET_UPVALUE:
        case OP_SET_UPVALUE:
        case OP_CALL:
        case OP_GET_FIELD:
        case OP_SET_FIELD:
        case OP_BUILD_ARRAY:
        case OP_BUILD_MAP:
        case OP_BUILD_TUPLE:
        case OP_PRINT:
        case OP_IMPORT:
        case OP_INC_LOCAL:
        case OP_DEC_LOCAL:
        case OP_LOAD_INT8:
        case OP_SET_INDEX_LOCAL:
        case OP_INDEX_LOCAL:
        case OP_SET_SLICE_LOCAL:
        case OP_APPEND_STR_LOCAL:
        case OP_REACT:
        case OP_UNREACT:
        case OP_BOND:
        case OP_UNBOND:
        case OP_SEED:
        case OP_UNSEED:
        case OP_DEFER_RUN: NEED(2); return 2;
        /* 3-byte */
        case OP_JUMP:
        case OP_JUMP_IF_FALSE:
        case OP_JUMP_IF_TRUE:
        case OP_JUMP_IF_NOT_NIL:
        case OP_LOOP:
        case OP_ITER_NEXT:
        case OP_PUSH_EXCEPTION_HANDLER:
        case OP_INVOKE:
        case OP_BUILD_STRUCT:
        case OP_GET_FIELD_LOCAL:
        case OP_CONSTANT_16:
        case OP_GET_GLOBAL_16:
        case OP_SET_GLOBAL_16:
        case OP_DEFINE_GLOBAL_16:
        case OP_INDEX_GLOBAL:
        case OP_SET_INDEX_GLOBAL:
        case OP_CHECK_RETURN_TYPE: NEED(3); return 3;
        /* 4-byte */
        case OP_BUILD_ENUM:
        case OP_INVOKE_LOCAL:
        case OP_INVOKE_GLOBAL:
        case OP_FREEZE_VAR:
        case OP_THAW_VAR:
        case OP_SUBLIMATE_VAR:
        case OP_FREEZE_FIELD:
        case OP_CHECK_TYPE:
        case OP_DEFER_PUSH: NEED(4); return 4;
        /* 5-byte */
        case OP_INVOKE_LOCAL_16:
        case OP_FREEZE_EXCEPT: NEED(5); return 5;
        /* 6-byte */
        case OP_INVOKE_GLOBAL_16: NEED(6); return 6;
        /* variable-length */
        case OP_CLOSURE: {
            NEED(3);
            size_t uvc = c->code[off + 2];
            NEED(3 + uvc * 2);
            return 3 + uvc * 2;
        }
        case OP_CLOSURE_16: {
            NEED(4);
            size_t uvc = c->code[off + 3];
            NEED(4 + uvc * 2);
            return 4 + uvc * 2;
        }
        case OP_SCOPE: {
            NEED(2);
            size_t spawn_count = c->code[off + 1];
            NEED(3 + spawn_count);
            return 3 + spawn_count;
        }
        case OP_SELECT: {
            NEED(2);
            size_t arm_count = c->code[off + 1];
            NEED(2 + arm_count * 4);
            return 2 + arm_count * 4;
        }
        default: *err = verify_failf("unknown opcode %u at offset %zu", op, off); return 0;
    }
#undef NEED
}

/* Validate the index/jump/type operands of the instruction at `off`. */
static char *verify_instr_operands(const Chunk *c, size_t off, const uint8_t *is_start) {
    uint8_t op = c->code[off];
#define U16(at) ((uint16_t)((c->code[(at)] << 8) | c->code[(at) + 1]))
#define CK_BOUND(idx)                                                                                            \
    do {                                                                                                         \
        if ((size_t)(idx) >= c->const_len)                                                                       \
            return verify_failf("constant index %u out of range (const_len=%zu) at offset %zu", (unsigned)(idx), \
                                c->const_len, off);                                                              \
    } while (0)
#define CK_STR(idx)                                                                                        \
    do {                                                                                                   \
        CK_BOUND(idx);                                                                                     \
        if (c->constants[(idx)].type != VAL_STR)                                                           \
            return verify_failf("opcode %u at offset %zu requires a string constant at index %u", op, off, \
                                (unsigned)(idx));                                                          \
    } while (0)
#define CK_SUBFN(idx)                                                                                        \
    do {                                                                                                     \
        CK_BOUND(idx);                                                                                       \
        if (c->constants[(idx)].type != VAL_CLOSURE)                                                         \
            return verify_failf("opcode %u at offset %zu requires a function constant at index %u", op, off, \
                                (unsigned)(idx));                                                            \
    } while (0)
    switch (op) {
        case OP_CONSTANT: CK_BOUND(c->code[off + 1]); break;
        case OP_CONSTANT_16: CK_BOUND(U16(off + 1)); break;
        case OP_GET_GLOBAL:
        case OP_SET_GLOBAL:
        case OP_DEFINE_GLOBAL:
        case OP_GET_FIELD:
        case OP_SET_FIELD:
        case OP_IMPORT:
        case OP_REACT:
        case OP_UNREACT:
        case OP_BOND:
        case OP_UNBOND:
        case OP_SEED:
        case OP_UNSEED: CK_STR(c->code[off + 1]); break;
        case OP_GET_GLOBAL_16:
        case OP_SET_GLOBAL_16:
        case OP_DEFINE_GLOBAL_16:
        case OP_INDEX_GLOBAL:
        case OP_SET_INDEX_GLOBAL: CK_STR(U16(off + 1)); break;
        case OP_INVOKE: CK_STR(c->code[off + 1]); break;
        case OP_INVOKE_LOCAL: CK_STR(c->code[off + 2]); break;
        case OP_INVOKE_GLOBAL:
            CK_STR(c->code[off + 1]);
            CK_STR(c->code[off + 2]);
            break;
        case OP_INVOKE_LOCAL_16: CK_STR(U16(off + 2)); break;
        case OP_INVOKE_GLOBAL_16:
            CK_STR(U16(off + 1));
            CK_STR(U16(off + 3));
            break;
        case OP_GET_FIELD_LOCAL: CK_STR(c->code[off + 2]); break;
        case OP_BUILD_STRUCT: {
            /* The VM reads constants[name_idx .. name_idx+field_count] as strings. */
            size_t name_idx = c->code[off + 1];
            size_t last = name_idx + c->code[off + 2];
            if (last >= c->const_len)
                return verify_failf("OP_BUILD_STRUCT field-name constants out of range at offset %zu", off);
            for (size_t k = name_idx; k <= last; k++)
                if (c->constants[k].type != VAL_STR)
                    return verify_failf("OP_BUILD_STRUCT requires string constants at offset %zu", off);
            break;
        }
        case OP_BUILD_ENUM:
            CK_STR(c->code[off + 1]);
            CK_STR(c->code[off + 2]);
            break;
        case OP_FREEZE_VAR:
        case OP_THAW_VAR:
        case OP_SUBLIMATE_VAR:
        case OP_FREEZE_FIELD:
        case OP_FREEZE_EXCEPT: CK_STR(c->code[off + 1]); break;
        case OP_CHECK_TYPE:
            CK_STR(c->code[off + 2]);
            CK_STR(c->code[off + 3]);
            break;
        case OP_CHECK_RETURN_TYPE:
            CK_STR(c->code[off + 1]);
            CK_STR(c->code[off + 2]);
            break;
        case OP_CLOSURE: CK_SUBFN(c->code[off + 1]); break;
        case OP_CLOSURE_16: CK_SUBFN(U16(off + 1)); break;
        case OP_SCOPE: {
            size_t spawn_count = c->code[off + 1];
            uint8_t sync_idx = c->code[off + 2];
            if (sync_idx != 0xFF) CK_SUBFN(sync_idx);
            for (size_t i = 0; i < spawn_count; i++) CK_SUBFN(c->code[off + 3 + i]);
            break;
        }
        case OP_SELECT: {
            size_t arm_count = c->code[off + 1];
            for (size_t i = 0; i < arm_count; i++) {
                size_t b = off + 2 + i * 4;
                uint8_t flags = c->code[b];
                uint8_t chan_idx = c->code[b + 1];
                uint8_t body_idx = c->code[b + 2];
                uint8_t binding_idx = c->code[b + 3];
                CK_SUBFN(body_idx);
                if (!(flags & 0x01)) CK_SUBFN(chan_idx); /* default arm has no channel */
                if (flags & 0x04) CK_STR(binding_idx);   /* arm binds the received value */
            }
            break;
        }
        case OP_JUMP:
        case OP_JUMP_IF_FALSE:
        case OP_JUMP_IF_TRUE:
        case OP_JUMP_IF_NOT_NIL:
        case OP_ITER_NEXT:
        case OP_PUSH_EXCEPTION_HANDLER: {
            size_t target = off + 3 + U16(off + 1);
            if (target >= c->code_len || !is_start[target])
                return verify_failf("forward jump target %zu invalid at offset %zu", target, off);
            break;
        }
        case OP_DEFER_PUSH: {
            /* [op][sdepth:1][offset:2]; the VM skips the inline defer body via
             * frame->ip += offset, so the skip target is off + 4 + offset. */
            size_t target = off + 4 + U16(off + 2);
            if (target >= c->code_len || !is_start[target])
                return verify_failf("defer skip target %zu invalid at offset %zu", target, off);
            break;
        }
        case OP_LOOP: {
            uint16_t back = U16(off + 1);
            if ((size_t)back > off + 3) return verify_failf("backward jump underflow at offset %zu", off);
            size_t target = off + 3 - back;
            if (target >= c->code_len || !is_start[target])
                return verify_failf("backward jump target %zu invalid at offset %zu", target, off);
            break;
        }
        default: break; /* opcodes with only slot/count/raw operands need no index check */
    }
    return NULL;
#undef U16
#undef CK_BOUND
#undef CK_STR
#undef CK_SUBFN
}

/* Verify the code stream of a single chunk (not its sub-chunks). */
static char *verify_chunk_code(const Chunk *c) {
    if (c->code_len == 0) return strdup("verify: empty chunk code");
    if (c->lines_len < c->code_len) return strdup("verify: line table shorter than code");

    uint8_t *is_start = calloc(c->code_len, 1);
    if (!is_start) return strdup("verify: out of memory");

    /* Pass 1: structural walk — opcode validity, operand lengths, boundaries. */
    size_t off = 0;
    uint8_t last_op = 0;
    while (off < c->code_len) {
        is_start[off] = 1;
        char *e = NULL;
        size_t len = verify_instr_length(c, off, &e);
        if (len == 0) {
            free(is_start);
            return e;
        }
        last_op = c->code[off];
        off += len;
    }
    if (off != c->code_len) {
        free(is_start);
        return strdup("verify: instruction crosses end of code");
    }
    if (last_op != OP_HALT && last_op != OP_RETURN && last_op != OP_THROW) {
        free(is_start);
        return strdup("verify: chunk does not end in a terminating instruction");
    }

    /* Pass 2: operand semantics (now that instruction boundaries are known). */
    off = 0;
    while (off < c->code_len) {
        char *e = verify_instr_operands(c, off, is_start);
        if (e) {
            free(is_start);
            return e;
        }
        char *ignore = NULL;
        off += verify_instr_length(c, off, &ignore); /* lengths already validated in pass 1 */
    }

    free(is_start);
    return NULL;
}

static char *chunk_verify_depth(const Chunk *c, int depth) {
    if (depth > CHUNK_VERIFY_MAX_DEPTH) return strdup("verify: chunk nesting too deep");
    char *e = verify_chunk_code(c);
    if (e) return e;
    for (size_t i = 0; i < c->const_len; i++) {
        if (c->constants[i].type == VAL_CLOSURE) {
            Chunk *sub = (Chunk *)c->constants[i].as.closure.native_fn;
            if (sub) {
                char *se = chunk_verify_depth(sub, depth + 1);
                if (se) return se;
            }
        }
    }
    return NULL;
}

char *chunk_verify(const Chunk *c) {
    if (!c) return strdup("verify: null chunk");
    return chunk_verify_depth(c, 0);
}
