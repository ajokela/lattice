#include "regvm.h"
#include "regopcode.h"
#include "ast.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>

/* ── Register compiler state ── */

typedef struct {
    char *name;
    int depth;
    bool is_captured;
    uint8_t reg; /* Which register this local lives in */
} RegLocal;

typedef struct {
    uint8_t index;
    bool is_local;
} RegCompilerUpvalue;

typedef enum { REG_FUNC_SCRIPT, REG_FUNC_FUNCTION, REG_FUNC_CLOSURE } RegFuncType;

typedef struct RegCompiler {
    struct RegCompiler *enclosing;
    RegChunk *chunk;
    RegFuncType type;
    char *func_name;
    int arity;
    RegLocal *locals;
    size_t local_count;
    size_t local_cap;
    RegCompilerUpvalue *upvalues;
    size_t upvalue_count;
    int scope_depth;
    uint8_t next_reg; /* Next available register */
    uint8_t max_reg;  /* High water mark for register usage */
    /* Break/continue tracking */
    size_t *break_patches; /* Instruction indices needing patching */
    size_t break_count;
    size_t break_cap;
    size_t *continue_patches; /* For-loop continue: forward jump patches */
    size_t continue_count;
    size_t continue_cap;
    size_t loop_start;
    int loop_is_for; /* 1 if innermost loop is for (continue uses forward jump) */
    int loop_depth;
    size_t loop_break_local_count;
    size_t loop_continue_local_count;
    uint8_t loop_break_reg; /* Register to restore to on break */
    uint8_t loop_continue_reg;
    int handler_depth; /* Active lexical try handlers in this function */
    int loop_break_scope_depth;
    int loop_continue_scope_depth;
    int loop_break_handler_depth;
    int loop_continue_handler_depth;
    /* Ensure contracts and return type for postcondition checking */
    ContractClause *ensures;
    size_t ensure_count;
    char *return_type;
    /* Selective imports are runtime values, so `Color::Red` cannot use the
     * compiler's declaration-only enum table. The root compiler records names
     * introduced by selective imports and nested compilers consult that set. */
    char **imported_enum_candidates;
    size_t imported_enum_candidate_count;
    size_t imported_enum_candidate_cap;
} RegCompiler;

static RegCompiler *rc = NULL; /* Current compiler */
static char *rc_error = NULL;

/* LAT-545: true only while compiling the top-level expression of a statement.
 * Captured/cleared at the top of compile_expr so only the outermost expression
 * observes it. Gates the nested-method-call write-back single-eval optimization
 * to the same statement-top contexts as the stack VM, keeping the two backends
 * consistent for the (rare) embedded-mutating-call case. */
static bool g_reg_stmt_tos = false;

/* Track known enums (same as stack compiler) */
typedef struct {
    char *name;
    char **variant_names;
    size_t *variant_counts;
    size_t variant_count;
} RegKnownEnum;
static RegKnownEnum *rc_known_enums = NULL;
static size_t rc_known_enum_count = 0;
static size_t rc_known_enum_cap = 0;

static void rc_register_enum(const EnumDecl *decl) {
    RegKnownEnum *entry = NULL;
    for (size_t i = 0; i < rc_known_enum_count; i++) {
        if (strcmp(rc_known_enums[i].name, decl->name) != 0) continue;
        entry = &rc_known_enums[i];
        for (size_t j = 0; j < entry->variant_count; j++) free(entry->variant_names[j]);
        free(entry->variant_names);
        free(entry->variant_counts);
        break;
    }
    if (!entry) {
        if (rc_known_enum_count >= rc_known_enum_cap) {
            rc_known_enum_cap = rc_known_enum_cap ? rc_known_enum_cap * 2 : 8;
            rc_known_enums = realloc(rc_known_enums, rc_known_enum_cap * sizeof(RegKnownEnum));
        }
        entry = &rc_known_enums[rc_known_enum_count++];
        entry->name = strdup(decl->name);
    }
    entry->variant_count = decl->variant_count;
    entry->variant_names = calloc(decl->variant_count, sizeof(char *));
    entry->variant_counts = calloc(decl->variant_count, sizeof(size_t));
    for (size_t i = 0; i < decl->variant_count; i++) {
        entry->variant_names[i] = strdup(decl->variants[i].name);
        entry->variant_counts[i] = decl->variants[i].param_count;
    }
}

static bool rc_is_known_enum(const char *name) {
    for (size_t i = 0; i < rc_known_enum_count; i++)
        if (strcmp(rc_known_enums[i].name, name) == 0) return true;
    return false;
}

static bool rc_known_enum_variant_ok(const char *enum_name, const char *variant_name, size_t argc) {
    for (size_t i = 0; i < rc_known_enum_count; i++) {
        if (strcmp(rc_known_enums[i].name, enum_name) != 0) continue;
        for (size_t j = 0; j < rc_known_enums[i].variant_count; j++)
            if (strcmp(rc_known_enums[i].variant_names[j], variant_name) == 0)
                return rc_known_enums[i].variant_counts[j] == argc;
        return false;
    }
    return true;
}

static RegCompiler *rc_root_compiler(void) {
    RegCompiler *root = rc;
    while (root && root->enclosing) root = root->enclosing;
    return root;
}

static void rc_register_imported_enum_candidate(const char *name) {
    RegCompiler *root = rc_root_compiler();
    if (!root) return;
    for (size_t i = 0; i < root->imported_enum_candidate_count; i++)
        if (strcmp(root->imported_enum_candidates[i], name) == 0) return;
    if (root->imported_enum_candidate_count >= root->imported_enum_candidate_cap) {
        size_t cap = root->imported_enum_candidate_cap ? root->imported_enum_candidate_cap * 2 : 8;
        char **names = realloc(root->imported_enum_candidates, cap * sizeof(char *));
        if (!names) {
            if (!rc_error) rc_error = strdup("out of memory tracking selective imports");
            return;
        }
        root->imported_enum_candidates = names;
        root->imported_enum_candidate_cap = cap;
    }
    root->imported_enum_candidates[root->imported_enum_candidate_count++] = strdup(name);
}

static bool rc_is_imported_enum_candidate(const char *name) {
    RegCompiler *root = rc_root_compiler();
    if (!root) return false;
    for (size_t i = 0; i < root->imported_enum_candidate_count; i++)
        if (strcmp(root->imported_enum_candidates[i], name) == 0) return true;
    return false;
}

/* Module enum metadata is an ordinary immutable-by-convention array:
 * [variant-name, payload-arity, ...]. Keeping it in the constant pool means
 * module maps and selective imports can transport it without a second metadata
 * registry or path-dependent compiler state. */
static LatValue rc_enum_metadata_value(const EnumDecl *decl) {
    size_t count = decl->variant_count * 2;
    LatValue *items = count ? malloc(count * sizeof(LatValue)) : NULL;
    if (count && !items) {
        if (!rc_error) rc_error = strdup("out of memory building enum metadata");
        return value_unit();
    }
    for (size_t i = 0; i < decl->variant_count; i++) {
        items[i * 2] = value_string(decl->variants[i].name);
        items[i * 2 + 1] = value_int((int64_t)decl->variants[i].param_count);
    }
    LatValue metadata = value_array(items, count);
    free(items);
    return metadata;
}

static void rc_free_known_enums(void) {
    for (size_t i = 0; i < rc_known_enum_count; i++) {
        for (size_t j = 0; j < rc_known_enums[i].variant_count; j++) free(rc_known_enums[i].variant_names[j]);
        free(rc_known_enums[i].variant_names);
        free(rc_known_enums[i].variant_counts);
        free(rc_known_enums[i].name);
    }
    free(rc_known_enums);
    rc_known_enums = NULL;
    rc_known_enum_count = 0;
    rc_known_enum_cap = 0;
}

/* ── Compiler init/cleanup ── */

static void rc_init(RegCompiler *comp, RegCompiler *enclosing, RegFuncType type) {
    comp->enclosing = enclosing;
    comp->chunk = regchunk_new();
    comp->type = type;
    comp->func_name = NULL;
    comp->arity = 0;
    comp->local_count = 0;
    comp->local_cap = 256;
    comp->locals = malloc(comp->local_cap * sizeof(RegLocal));
    if (!comp->locals) return;
    comp->upvalues = NULL;
    comp->upvalue_count = 0;
    comp->scope_depth = (type == REG_FUNC_SCRIPT) ? 0 : 1;
    comp->next_reg = 0;
    comp->max_reg = 0;
    comp->break_patches = NULL;
    comp->break_count = 0;
    comp->break_cap = 0;
    comp->continue_patches = NULL;
    comp->continue_count = 0;
    comp->continue_cap = 0;
    comp->loop_start = 0;
    comp->loop_is_for = 0;
    comp->loop_depth = 0;
    comp->loop_break_local_count = 0;
    comp->loop_continue_local_count = 0;
    comp->loop_break_reg = 0;
    comp->loop_continue_reg = 0;
    comp->handler_depth = 0;
    comp->loop_break_scope_depth = 0;
    comp->loop_continue_scope_depth = 0;
    comp->loop_break_handler_depth = 0;
    comp->loop_continue_handler_depth = 0;
    comp->ensures = NULL;
    comp->ensure_count = 0;
    comp->return_type = NULL;
    comp->imported_enum_candidates = NULL;
    comp->imported_enum_candidate_count = 0;
    comp->imported_enum_candidate_cap = 0;

    /* Reserve register 0 for function itself (convention) */
    if (type != REG_FUNC_SCRIPT) {
        RegLocal *local = &comp->locals[comp->local_count++];
        local->name = strdup("");
        local->depth = 0;
        local->is_captured = false;
        local->reg = comp->next_reg++;
    }

    rc = comp;
}

static void rc_cleanup(RegCompiler *comp) {
    for (size_t i = 0; i < comp->local_count; i++) free(comp->locals[i].name);
    free(comp->locals);
    free(comp->upvalues);
    free(comp->break_patches);
    free(comp->continue_patches);
    free(comp->func_name);
    for (size_t i = 0; i < comp->imported_enum_candidate_count; i++) free(comp->imported_enum_candidates[i]);
    free(comp->imported_enum_candidates);
}

/* ── Register management ── */

static uint8_t alloc_reg(void) {
    if (rc_error) return 0;
    if (rc->next_reg >= REGVM_REG_MAX - 1) {
        rc_error = strdup("register overflow (>256 registers)");
        return 0;
    }
    uint8_t r = rc->next_reg++;
    if (r >= rc->max_reg) {
        rc->max_reg = r + 1;
        if (rc->chunk) rc->chunk->max_reg = rc->max_reg;
    }
    return r;
}

static void free_reg(uint8_t r) {
    /* Only free if it's the top-most register (simple stack-like alloc) */
    if (r == rc->next_reg - 1) rc->next_reg--;
}

static void free_regs_to(uint8_t target) {
    if (rc->next_reg > target) rc->next_reg = target;
}

/* ── Emit helpers ── */

static RegChunk *current_chunk(void) { return rc->chunk; }

static size_t emit(RegInstr instr, int line) {
    if (rc_error) return 0;
    return regchunk_write(current_chunk(), instr, line);
}

static size_t emit_ABx(uint8_t op, uint8_t a, uint16_t bx, int line) { return emit(REG_ENCODE_ABx(op, a, bx), line); }

static size_t emit_ABC(uint8_t op, uint8_t a, uint8_t b, uint8_t c, int line) {
    return emit(REG_ENCODE_ABC(op, a, b, c), line);
}

static size_t emit_AsBx(uint8_t op, uint8_t a, int16_t sbx, int line) {
    return emit(REG_ENCODE_AsBx(op, a, sbx), line);
}

/* Emit a GETFIELD whose field-name key is `field_ki`. Uses the compact
 * single-word form when the constant index fits in 8 bits, otherwise the
 * two-word ROP_GETFIELD_16 form (LAT-463: never truncate the index). */
static void emit_getfield(uint8_t dst, uint8_t obj, uint16_t field_ki, int line) {
    if (field_ki <= 0xFF) {
        emit(REG_ENCODE_ABC(ROP_GETFIELD, dst, obj, (uint8_t)field_ki), line);
    } else {
        emit(REG_ENCODE_ABC(ROP_GETFIELD_16, dst, obj, 0), line);
        emit(REG_ENCODE_ABx(0, 0, field_ki), line); /* data word: Bx = field_ki */
    }
}

/* Emit a SETFIELD whose field-name key is `field_ki`. Wide form when needed. */
static void emit_setfield(uint8_t obj, uint16_t field_ki, uint8_t val, int line) {
    if (field_ki <= 0xFF) {
        emit(REG_ENCODE_ABC(ROP_SETFIELD, obj, (uint8_t)field_ki, val), line);
    } else {
        emit(REG_ENCODE_ABC(ROP_SETFIELD_16, obj, 0, val), line);
        emit(REG_ENCODE_ABx(0, 0, field_ki), line); /* data word: Bx = field_ki */
    }
}

/* Emit jump placeholder, return instruction index for patching */
static size_t emit_jump_placeholder(uint8_t op, uint8_t a, int line) { return emit_AsBx(op, a, 0, line); }

/* Two-word default check. A=0xff marks the conditional as argument-presence
 * based; the data word carries the parameter register. */
static size_t emit_default_skip(uint8_t param_reg, int line) {
    size_t jump = emit_jump_placeholder(ROP_JMPNOTNIL, 0xff, line);
    emit_ABC(ROP_MOVE, param_reg, 0, 0, line); /* data word, consumed by VM */
    return jump;
}

static size_t emit_jmp_placeholder(int line) { return emit(REG_ENCODE_sBx(ROP_JMP, 0), line); }

/* Patch a conditional jump (AsBx format, 16-bit signed offset) */
static void patch_jump(size_t instr_idx) {
    if (rc_error) return;
    long off = (long)current_chunk()->code_len - (long)instr_idx - 1;
    /* LAT-485: refuse to truncate an offset that does not fit in int16_t —
     * a silently wrapped offset jumps to an arbitrary instruction. */
    if (off < -32768 || off > 32767) {
        rc_error = strdup("jump offset out of range (function body exceeds 32767 instructions)");
        return;
    }
    int16_t offset = (int16_t)off;
    RegInstr old = current_chunk()->code[instr_idx];
    uint8_t op = REG_GET_OP(old);
    uint8_t a = REG_GET_A(old);
    current_chunk()->code[instr_idx] = REG_ENCODE_AsBx(op, a, offset);
}

/* Patch an unconditional jump (sBx24 format, 24-bit signed offset) */
static void patch_jmp(size_t instr_idx) {
    if (rc_error) return;
    long off = (long)current_chunk()->code_len - (long)instr_idx - 1;
    /* LAT-485: 24-bit signed range; refuse rather than wrap. */
    if (off < -0x7FFFFF || off > 0x7FFFFF) {
        rc_error = strdup("jump offset out of range (24-bit limit exceeded)");
        return;
    }
    /* Preserve original opcode (may be ROP_JMP, ROP_DEFER_PUSH, etc.) */
    uint8_t op = REG_GET_OP(current_chunk()->code[instr_idx]);
    current_chunk()->code[instr_idx] = REG_ENCODE_sBx(op, (int32_t)off);
}

/* Emit backward jump to loop_start */
static void emit_loop_back(size_t loop_start, int line) {
    if (rc_error) return;
    long off = (long)loop_start - (long)current_chunk()->code_len - 1;
    /* LAT-485: 24-bit signed range; refuse rather than wrap. */
    if (off < -0x7FFFFF || off > 0x7FFFFF) {
        rc_error = strdup("loop jump offset out of range (24-bit limit exceeded)");
        return;
    }
    emit(REG_ENCODE_sBx(ROP_JMP, (int32_t)off), line);
}

/* Emit DEFER_RUN + RETURN (use for all returns except inside defer bodies) */
static void emit_return(uint8_t reg, int line) {
    emit(REG_ENCODE_ABC(ROP_DEFER_RUN, 0, 0, 0), line);
    emit(REG_ENCODE_ABC(ROP_RETURN, reg, 1, 0), line);
}

/* Forward declaration for ensure checks (defined after compile_expr) */
static void emit_postconditions(uint8_t reg, int line);

/* ── Constant pool ── */

static uint16_t add_constant(LatValue val) {
    if (rc_error) return 0;
    size_t idx = regchunk_add_constant(current_chunk(), val);
    if (idx >= REGVM_CONST_MAX) {
        rc_error = strdup("too many constants in one chunk (>65535)");
        return 0;
    }
    return (uint16_t)idx;
}

static void emit_assert_reg(uint8_t condition, const char *message, int line) {
    size_t ok = emit_jump_placeholder(ROP_JMPTRUE, condition, line);
    uint8_t err_reg = alloc_reg();
    emit_ABx(ROP_LOADK, err_reg, add_constant(value_string(message)), line);
    emit_ABC(ROP_THROW, err_reg, 0, 0, line);
    free_reg(err_reg);
    patch_jump(ok);
}

/* Pack a name/string constant index into an 8-bit instruction operand.
 * Several opcodes (FREEZE_VAR/THAW_VAR/SUBLIMATE_VAR, FREEZE_EXCEPT,
 * FREEZE_FIELD, the INVOKE_GLOBAL global name, the NEWENUM variant name) have
 * no spare bits for a wide index, so rather than silently truncating a >255
 * index — which would make the VM dereference an unrelated constant as a
 * char* (LAT-463) — we refuse to emit and surface a compile error. */
static uint8_t name_ki8(uint16_t ki) {
    if (rc_error) return 0;
    if (ki > 0xFF) {
        rc_error = strdup("too many constants in one chunk for an 8-bit name "
                          "operand (split the function or reduce literals)");
        return 0;
    }
    return (uint8_t)ki;
}

/* Like name_ki8 but reserves 0xFF as a sentinel (scope/select sub-chunk and
 * binding indices use 0xFF to mean "none"), so the safe range is 0..254. */
static uint8_t sub_idx8(uint16_t ki) {
    if (rc_error) return 0;
    if (ki >= 0xFF) {
        rc_error = strdup("too many constants in one chunk for a scope/select "
                          "sub-chunk index (>254)");
        return 0;
    }
    return (uint8_t)ki;
}

/* ── Scope and local management ── */

static void begin_scope(void) { rc->scope_depth++; }

static void end_scope(int line) {
    /* Run defers registered at this scope depth (block-scoped defers) */
    if (rc->scope_depth > 1) { emit_ABC(ROP_DEFER_RUN, (uint8_t)rc->scope_depth, 0, 0, line); }
    rc->scope_depth--;
    while (rc->local_count > 0 && rc->locals[rc->local_count - 1].depth > rc->scope_depth) {
        RegLocal *local = &rc->locals[rc->local_count - 1];
        if (local->is_captured) { emit_ABC(ROP_CLOSEUPVALUE, local->reg, 0, 0, line); }
        /* Free the register */
        free_reg(local->reg);
        free(local->name);
        rc->local_count--;
    }
}

static uint8_t add_local(const char *name) {
    if (rc->local_count >= rc->local_cap) {
        rc->local_cap *= 2;
        rc->locals = realloc(rc->locals, rc->local_cap * sizeof(RegLocal));
    }
    uint8_t reg = alloc_reg();
    RegLocal *local = &rc->locals[rc->local_count++];
    local->name = strdup(name);
    local->depth = rc->scope_depth;
    local->is_captured = false;
    local->reg = reg;
    regchunk_set_local_name(current_chunk(), reg, name);
    return reg;
}

static int resolve_local(RegCompiler *comp, const char *name) {
    for (int i = (int)comp->local_count - 1; i >= 0; i--) {
        if (strcmp(comp->locals[i].name, name) == 0) return i;
    }
    return -1;
}

static uint8_t local_reg(int local_idx) { return rc->locals[local_idx].reg; }

/* ── Upvalue resolution ── */

static int add_upvalue(RegCompiler *comp, uint8_t index, bool is_local) {
    for (size_t i = 0; i < comp->upvalue_count; i++) {
        if (comp->upvalues[i].index == index && comp->upvalues[i].is_local == is_local) return (int)i;
    }
    if (comp->upvalue_count >= 256) {
        rc_error = strdup("too many upvalues in one function");
        return -1;
    }
    comp->upvalues = realloc(comp->upvalues, (comp->upvalue_count + 1) * sizeof(RegCompilerUpvalue));
    comp->upvalues[comp->upvalue_count].index = index;
    comp->upvalues[comp->upvalue_count].is_local = is_local;
    return (int)comp->upvalue_count++;
}

static int resolve_upvalue(RegCompiler *comp, const char *name) {
    if (!comp->enclosing) return -1;
    int local = resolve_local(comp->enclosing, name);
    if (local != -1) {
        comp->enclosing->locals[local].is_captured = true;
        return add_upvalue(comp, comp->enclosing->locals[local].reg, true);
    }
    int upvalue = resolve_upvalue(comp->enclosing, name);
    if (upvalue != -1) return add_upvalue(comp, (uint8_t)upvalue, false);
    return -1;
}

/* ── Break/continue helpers ── */

static void push_break_patch(size_t instr_idx) {
    if (rc->break_count >= rc->break_cap) {
        rc->break_cap = rc->break_cap ? rc->break_cap * 2 : 8;
        rc->break_patches = realloc(rc->break_patches, rc->break_cap * sizeof(size_t));
    }
    rc->break_patches[rc->break_count++] = instr_idx;
}

static void push_continue_patch(size_t instr_idx) {
    if (rc->continue_count >= rc->continue_cap) {
        rc->continue_cap = rc->continue_cap ? rc->continue_cap * 2 : 8;
        rc->continue_patches = realloc(rc->continue_patches, rc->continue_cap * sizeof(size_t));
    }
    rc->continue_patches[rc->continue_count++] = instr_idx;
}

/* Run cleanup for lexical scopes and try handlers crossed by a loop-control
 * transfer. Defers run while their try handler is still active, so an error in
 * deferred work follows the same catch semantics as normal scope exit. */
static void emit_loop_control_cleanup(size_t keep_local_count, int target_scope_depth, int target_handler_depth,
                                      int line) {
    if (rc->scope_depth > target_scope_depth) {
        emit_ABC(ROP_DEFER_RUN, (uint8_t)(target_scope_depth + 1), 0, 0, line);
    }
    for (size_t i = rc->local_count; i > keep_local_count; i--) {
        RegLocal *local = &rc->locals[i - 1];
        if (local->is_captured) emit_ABC(ROP_CLOSEUPVALUE, local->reg, 0, 0, line);
    }
    for (int depth = rc->handler_depth; depth > target_handler_depth; depth--) {
        emit_ABC(ROP_POP_HANDLER, 0, 0, 0, line);
    }
}

/* ── Forward declarations ── */

static void compile_expr(const Expr *e, uint8_t dst, int line);
static void compile_stmt(const Stmt *s);

static void emit_check_enum_variant(uint8_t metadata_reg, const char *variant_name, size_t argc, int line) {
    if (argc > UINT8_MAX) {
        if (!rc_error) rc_error = strdup("enum payload has more than 255 values");
        return;
    }
    uint16_t variant_ki = add_constant(value_string(variant_name));
    emit_ABC(ROP_CHECK_ENUM_VARIANT, metadata_reg, (uint8_t)argc, 0, line);
    emit_ABx(0, 0, variant_ki, line);
}

static void compile_enum_construction(const Expr *e, uint8_t dst, int line) {
    if (e->as.enum_variant.arg_count == 0) {
        emit_ABx(ROP_LOADK, dst,
                 add_constant(value_enum(e->as.enum_variant.enum_name, e->as.enum_variant.variant_name, NULL, 0)),
                 line);
        return;
    }

    uint8_t base = alloc_reg();
    for (size_t i = 0; i < e->as.enum_variant.arg_count; i++) {
        uint8_t arg_reg = i == 0 ? base : alloc_reg();
        compile_expr(e->as.enum_variant.args[i], arg_reg, line);
    }
    uint16_t name_ki = add_constant(value_string(e->as.enum_variant.enum_name));
    uint16_t variant_ki = add_constant(value_string(e->as.enum_variant.variant_name));
    emit_ABC(ROP_NEWENUM, dst, (uint8_t)(name_ki & 0xff), (uint8_t)e->as.enum_variant.arg_count, line);
    emit_ABC(ROP_MOVE, base, name_ki8(variant_ki), (uint8_t)(name_ki >> 8), line);
    free_regs_to(base);
}

typedef struct {
    size_t *items;
    size_t count;
    size_t cap;
} RegPatternFailList;

static void pattern_fail_add(RegPatternFailList *fails, size_t jump) {
    if (rc_error) return;
    if (fails->count >= fails->cap) {
        size_t new_cap = fails->cap ? fails->cap * 2 : 8;
        size_t *grown = realloc(fails->items, new_cap * sizeof(size_t));
        if (!grown) {
            rc_error = strdup("out of memory compiling match pattern");
            return;
        }
        fails->items = grown;
        fails->cap = new_cap;
    }
    fails->items[fails->count++] = jump;
}

static void pattern_fail_if_false(RegPatternFailList *fails, uint8_t condition, int line) {
    pattern_fail_add(fails, emit_jump_placeholder(ROP_JMPFALSE, condition, line));
}

static void emit_match_field(uint8_t present, uint8_t value, uint8_t object, uint16_t field_ki, int line) {
    emit_ABC(ROP_MATCH_FIELD, present, value, object, line);
    emit_ABx(0, 0, field_ki, line);
}

static void emit_pattern_size(uint8_t dst, size_t value, int line) {
    if (value <= INT16_MAX) {
        emit_AsBx(ROP_LOADI, dst, (int16_t)value, line);
    } else if (value <= INT64_MAX) {
        emit_ABx(ROP_LOADK, dst, add_constant(value_int((int64_t)value)), line);
    } else if (!rc_error) {
        rc_error = strdup("match pattern is too large");
    }
}

/* Bindings are allocated before validation so every temporary produced by the
 * recursive checker remains above them and can be reclaimed normally. */
static void declare_pattern_bindings(const Pattern *pat) {
    switch (pat->tag) {
        case PAT_BINDING: add_local(pat->as.binding_name); break;
        case PAT_ARRAY:
            for (size_t i = 0; i < pat->as.array.count; i++) declare_pattern_bindings(pat->as.array.elems[i].pattern);
            break;
        case PAT_STRUCT:
            for (size_t i = 0; i < pat->as.strct.count; i++) {
                StructPatField *field = &pat->as.strct.fields[i];
                if (field->value_pat) declare_pattern_bindings(field->value_pat);
                else add_local(field->name);
            }
            break;
        case PAT_ENUM_VARIANT:
            for (size_t i = 0; i < pat->as.enum_variant.payload_count; i++)
                declare_pattern_bindings(pat->as.enum_variant.payload_pats[i]);
            break;
        case PAT_LITERAL:
        case PAT_WILDCARD:
        case PAT_RANGE: break;
    }
}

static uint8_t pattern_binding_reg(const char *name) {
    int local = resolve_local(rc, name);
    if (local < 0) {
        if (!rc_error) rc_error = strdup("internal error: missing match binding");
        return 0;
    }
    return local_reg(local);
}

/* Recursively validate one pattern and extract its predeclared bindings. Every
 * failure path produces false rather than evaluating an unsafe child access. */
static void compile_pattern_match(const Pattern *pat, uint8_t candidate, uint8_t result, int line) {
    RegPatternFailList fails = {0};

    switch (pat->tag) {
        case PAT_WILDCARD: break;

        case PAT_BINDING: emit_ABC(ROP_MOVE, pattern_binding_reg(pat->as.binding_name), candidate, 0, line); break;

        case PAT_LITERAL: {
            uint8_t expected = alloc_reg();
            compile_expr(pat->as.literal, expected, line);
            emit_ABC(ROP_EQ, result, candidate, expected, line);
            pattern_fail_if_false(&fails, result, line);
            free_reg(expected);
            break;
        }

        case PAT_RANGE: {
            uint8_t start = alloc_reg();
            compile_expr(pat->as.range.start, start, line);
            uint8_t end = alloc_reg();
            compile_expr(pat->as.range.end, end, line);

            emit_ABC(ROP_MATCH_TYPE, result, candidate, VAL_INT, line);
            pattern_fail_if_false(&fails, result, line);
            emit_ABC(ROP_MATCH_TYPE, result, start, VAL_INT, line);
            pattern_fail_if_false(&fails, result, line);
            emit_ABC(ROP_MATCH_TYPE, result, end, VAL_INT, line);
            pattern_fail_if_false(&fails, result, line);
            emit_ABC(ROP_GTEQ, result, candidate, start, line);
            pattern_fail_if_false(&fails, result, line);
            emit_ABC(ROP_LTEQ, result, candidate, end, line);
            pattern_fail_if_false(&fails, result, line);

            free_reg(end);
            free_reg(start);
            break;
        }

        case PAT_ARRAY: {
            ArrayPatElem *elements = pat->as.array.elems;
            size_t pattern_count = pat->as.array.count;
            size_t fixed_count = 0;
            int rest_index = -1;
            bool multiple_rests = false;
            for (size_t i = 0; i < pattern_count; i++) {
                if (elements[i].is_rest) {
                    if (rest_index >= 0) multiple_rests = true;
                    rest_index = (int)i;
                } else {
                    fixed_count++;
                }
            }
            if (multiple_rests) {
                emit_ABC(ROP_LOADFALSE, result, 0, 0, line);
                pattern_fail_if_false(&fails, result, line);
                break;
            }

            emit_ABC(ROP_MATCH_TYPE, result, candidate, VAL_ARRAY, line);
            pattern_fail_if_false(&fails, result, line);

            uint8_t length = alloc_reg();
            emit_ABC(ROP_LEN, length, candidate, 0, line);
            uint8_t expected = alloc_reg();
            emit_pattern_size(expected, fixed_count, line);
            emit_ABC(rest_index >= 0 ? ROP_GTEQ : ROP_EQ, result, length, expected, line);
            pattern_fail_if_false(&fails, result, line);
            free_reg(expected);

            for (size_t i = 0; i < pattern_count; i++) {
                uint8_t child;
                if (elements[i].is_rest) {
                    size_t after_count = pattern_count - i - 1;
                    uint8_t start = alloc_reg();
                    emit_pattern_size(start, i, line);
                    uint8_t end = alloc_reg();
                    if (after_count > 0) {
                        uint8_t after = alloc_reg();
                        emit_pattern_size(after, after_count, line);
                        emit_ABC(ROP_SUB, end, length, after, line);
                        free_reg(after);
                    } else {
                        emit_ABC(ROP_MOVE, end, length, 0, line);
                    }
                    uint8_t range = alloc_reg();
                    emit_ABC(ROP_BUILDRANGE, range, start, end, line);
                    child = alloc_reg();
                    emit_ABC(ROP_GETINDEX, child, candidate, range, line);
                    compile_pattern_match(elements[i].pattern, child, result, line);
                    pattern_fail_if_false(&fails, result, line);
                    free_reg(child);
                    free_reg(range);
                    free_reg(end);
                    free_reg(start);
                    continue;
                }

                uint8_t index = alloc_reg();
                if (rest_index >= 0 && (int)i > rest_index) {
                    uint8_t offset = alloc_reg();
                    emit_pattern_size(offset, pattern_count - i, line);
                    emit_ABC(ROP_SUB, index, length, offset, line);
                    free_reg(offset);
                } else {
                    emit_pattern_size(index, i, line);
                }
                child = alloc_reg();
                emit_ABC(ROP_GETINDEX, child, candidate, index, line);
                compile_pattern_match(elements[i].pattern, child, result, line);
                pattern_fail_if_false(&fails, result, line);
                free_reg(child);
                free_reg(index);
            }
            free_reg(length);
            break;
        }

        case PAT_STRUCT: {
            emit_ABC(ROP_MATCH_TYPE, result, candidate, VAL_STRUCT, line);
            pattern_fail_if_false(&fails, result, line);

            for (size_t i = 0; i < pat->as.strct.count; i++) {
                StructPatField *field = &pat->as.strct.fields[i];
                uint16_t field_ki = add_constant(value_string(field->name));
                uint8_t field_value = alloc_reg();
                uint8_t present = alloc_reg();
                emit_match_field(present, field_value, candidate, field_ki, line);
                pattern_fail_if_false(&fails, present, line);
                free_reg(present);

                if (field->value_pat) {
                    compile_pattern_match(field->value_pat, field_value, result, line);
                    pattern_fail_if_false(&fails, result, line);
                } else {
                    emit_ABC(ROP_MOVE, pattern_binding_reg(field->name), field_value, 0, line);
                }
                free_reg(field_value);
            }
            break;
        }

        case PAT_ENUM_VARIANT: {
            emit_ABC(ROP_MATCH_TYPE, result, candidate, VAL_ENUM, line);
            pattern_fail_if_false(&fails, result, line);

            uint8_t actual = alloc_reg();
            emit_getfield(actual, candidate, add_constant(value_string("enum_name")), line);
            uint8_t expected = alloc_reg();
            emit_ABx(ROP_LOADK, expected, add_constant(value_string(pat->as.enum_variant.enum_name)), line);
            emit_ABC(ROP_EQ, result, actual, expected, line);
            pattern_fail_if_false(&fails, result, line);
            free_reg(expected);
            free_reg(actual);

            actual = alloc_reg();
            emit_getfield(actual, candidate, add_constant(value_string("variant_name")), line);
            expected = alloc_reg();
            emit_ABx(ROP_LOADK, expected, add_constant(value_string(pat->as.enum_variant.variant_name)), line);
            emit_ABC(ROP_EQ, result, actual, expected, line);
            pattern_fail_if_false(&fails, result, line);
            free_reg(expected);
            free_reg(actual);

            uint8_t payload = alloc_reg();
            emit_getfield(payload, candidate, add_constant(value_string("payload")), line);
            uint8_t payload_len = alloc_reg();
            emit_ABC(ROP_LEN, payload_len, payload, 0, line);
            expected = alloc_reg();
            emit_pattern_size(expected, pat->as.enum_variant.payload_count, line);
            emit_ABC(ROP_EQ, result, payload_len, expected, line);
            pattern_fail_if_false(&fails, result, line);
            free_reg(expected);
            free_reg(payload_len);

            for (size_t i = 0; i < pat->as.enum_variant.payload_count; i++) {
                uint8_t index = alloc_reg();
                emit_pattern_size(index, i, line);
                uint8_t child = alloc_reg();
                emit_ABC(ROP_GETINDEX, child, payload, index, line);
                compile_pattern_match(pat->as.enum_variant.payload_pats[i], child, result, line);
                pattern_fail_if_false(&fails, result, line);
                free_reg(child);
                free_reg(index);
            }
            free_reg(payload);
            break;
        }
    }

    if (pat->phase_qualifier != PHASE_UNSPECIFIED) {
        emit_ABC(ROP_MATCH_PHASE, result, candidate, (uint8_t)pat->phase_qualifier, line);
        pattern_fail_if_false(&fails, result, line);
    }

    emit_ABC(ROP_LOADTRUE, result, 0, 0, line);
    size_t done = emit_jmp_placeholder(line);
    for (size_t i = 0; i < fails.count; i++) patch_jump(fails.items[i]);
    emit_ABC(ROP_LOADFALSE, result, 0, 0, line);
    patch_jmp(done);
    free(fails.items);
}
static void emit_index_writeback(Expr *node, uint8_t modified_reg, int line);

/* ── LAT-545: single-evaluation of non-idempotent indices in write-backs ──
 * See the matching comment in src/stackcompiler.c. A nested mutation reads the
 * receiver/parent (evaluating each index level) and writes it back through the
 * same chain (re-evaluating them); for a side-effecting index the read and
 * write-back target different slots and the effect fires twice. Fix: evaluate
 * each non-idempotent index level once into a register, bind it to a synthetic
 * identifier, and rewrite the AST index child so read + write-back reuse it. */

static bool index_expr_idempotent(const Expr *ix) {
    switch (ix->tag) {
        case EXPR_INT_LIT:
        case EXPR_FLOAT_LIT:
        case EXPR_BOOL_LIT:
        case EXPR_STRING_LIT:
        case EXPR_NIL_LIT:
        case EXPR_IDENT: return true;
        default: return false;
    }
}

/* True if any index level of chain `chain` is non-idempotent. */
static bool chain_has_impure_index(const Expr *chain) {
    for (const Expr *n = chain; n && n->tag == EXPR_INDEX; n = n->as.index.object) {
        if (!index_expr_idempotent(n->as.index.index)) return true;
    }
    return false;
}

#define IDX_HOIST_MAX 16
static unsigned g_reg_idx_hoist_counter = 0;

typedef struct {
    Expr *nodes[IDX_HOIST_MAX]; /* EXPR_INDEX node whose index child was rewritten */
    Expr *orig[IDX_HOIST_MAX];  /* saved original index child */
    Expr idents[IDX_HOIST_MAX]; /* synthetic replacement EXPR_IDENT nodes */
    char names[IDX_HOIST_MAX][32];
    int count;
    size_t saved_local_count;
    uint8_t saved_next_reg;
} RegIdxHoist;

/* Evaluate each non-idempotent index level of `chain` once into a fresh register
 * (bound to a unique synthetic-name local) and rewrite the AST index child to a
 * synthetic EXPR_IDENT referencing it. reg_idx_hoist_end restores the AST, pops
 * the synthetic locals, and reclaims the registers. */
static void reg_idx_hoist_begin(RegIdxHoist *h, Expr *chain, int line) {
    h->count = 0;
    h->saved_local_count = rc->local_count;
    h->saved_next_reg = rc->next_reg;
    unsigned tag = g_reg_idx_hoist_counter++;
    Expr *chstack[IDX_HOIST_MAX];
    int n = 0;
    for (Expr *nd = chain; nd && nd->tag == EXPR_INDEX && n < IDX_HOIST_MAX; nd = nd->as.index.object)
        chstack[n++] = nd;
    for (int i = n - 1; i >= 0; i--) {
        Expr *nd = chstack[i];
        if (index_expr_idempotent(nd->as.index.index)) continue;
        if (h->count >= IDX_HOIST_MAX) break;
        int k = h->count;
        snprintf(h->names[k], sizeof(h->names[k]), "$idxh%u_%d", tag, k);
        uint8_t reg = add_local(h->names[k]);        /* dedicated register, held until end */
        compile_expr(nd->as.index.index, reg, line); /* evaluate ONCE */
        h->idents[k].tag = EXPR_IDENT;
        h->idents[k].line = line;
        h->idents[k].as.str_val = h->names[k];
        h->nodes[k] = nd;
        h->orig[k] = nd->as.index.index;
        nd->as.index.index = &h->idents[k];
        h->count++;
    }
}

static void reg_idx_hoist_end(RegIdxHoist *h) {
    for (int k = 0; k < h->count; k++) h->nodes[k]->as.index.index = h->orig[k];
    while (rc->local_count > h->saved_local_count) free(rc->locals[--rc->local_count].name);
    free_regs_to(h->saved_next_reg);
}

/* True when an EXPR_INDEX chain bottoms out at a named variable — the only base
 * emit_index_writeback can actually store back to. For a non-writable base (a
 * call result, a field access, ...) the write-back chain emits nothing at its
 * terminal and would only re-evaluate the base expression, so the LAT-544
 * nested-element mutation must fall back to the plain INVOKE path (a correct
 * no-op) instead. Mirrors stackcompiler's gate so regvm and stackvm agree. */
static bool index_chain_writable(const Expr *node) {
    const Expr *o = node->as.index.object;
    if (o->tag == EXPR_IDENT) return true;
    if (o->tag == EXPR_INDEX) return index_chain_writable(o);
    return false;
}

/* Compile statements into a standalone RegChunk (sub-body for scope/spawn/select).
 * The last expression statement becomes the return value; otherwise returns unit. */
static RegChunk *compile_reg_sub_body(Stmt **stmts, size_t count, int line) {
    RegCompiler *saved = rc;
    RegCompiler sub;
    rc_init(&sub, NULL, REG_FUNC_SCRIPT);
    sub.scope_depth = 1; /* treat as local scope */
    rc = &sub;
    /* Slot 0 reserved (convention) */
    add_local("");

    if (count > 0 && stmts[count - 1]->tag == STMT_EXPR) {
        for (size_t i = 0; i + 1 < count; i++) compile_stmt(stmts[i]);
        uint8_t result_reg = alloc_reg();
        compile_expr(stmts[count - 1]->as.expr, result_reg, line);
        emit_return(result_reg, line);
        free_reg(result_reg);
    } else {
        for (size_t i = 0; i < count; i++) compile_stmt(stmts[i]);
        uint8_t result_reg = alloc_reg();
        emit_ABC(ROP_LOADUNIT, result_reg, 0, 0, line);
        emit_return(result_reg, line);
        free_reg(result_reg);
    }

    RegChunk *ch = sub.chunk;
    sub.chunk = NULL; /* prevent rc_cleanup from freeing it */
    rc_cleanup(&sub);
    rc = saved;
    return ch;
}

/* Store a sub-RegChunk as a VAL_CLOSURE constant (body=NULL, native_fn=RegChunk*). */
static uint16_t add_regchunk_constant(RegChunk *ch) {
    LatValue fn_val;
    memset(&fn_val, 0, sizeof(fn_val));
    fn_val.type = VAL_CLOSURE;
    fn_val.phase = VTAG_UNPHASED;
    fn_val.region_id = (size_t)-1;
    fn_val.as.closure.body = NULL;
    fn_val.as.closure.native_fn = (void *)ch;
    return add_constant(fn_val);
}

/* ── Expression compilation ──
 * Each expression compiles its result into register `dst`.
 */

/* ── Recursive constant folding ──
 * Attempts to evaluate an expression at compile time.
 * Returns true and sets *out if the entire expression tree is constant.
 * Returns false for non-constant expressions (fall through to normal compilation).
 * Skips AND/OR/NIL_COALESCE (short-circuit semantics) and div-by-zero (runtime error). */
static bool try_const_fold(const Expr *e, LatValue *out) {
    if (!e) return false;
    switch (e->tag) {
        case EXPR_INT_LIT: *out = value_int(e->as.int_val); return true;
        case EXPR_FLOAT_LIT: *out = value_float(e->as.float_val); return true;
        case EXPR_BOOL_LIT: *out = value_bool(e->as.bool_val); return true;
        case EXPR_STRING_LIT: *out = value_string(e->as.str_val); return true;
        case EXPR_NIL_LIT: *out = value_nil(); return true;

        case EXPR_UNARYOP: {
            LatValue operand;
            if (!try_const_fold(e->as.unaryop.operand, &operand)) return false;
            switch (e->as.unaryop.op) {
                case UNOP_NEG:
                    if (operand.type == VAL_INT) {
                        *out = value_int(-operand.as.int_val);
                        return true;
                    }
                    if (operand.type == VAL_FLOAT) {
                        *out = value_float(-operand.as.float_val);
                        return true;
                    }
                    break;
                case UNOP_NOT:
                    if (operand.type == VAL_BOOL) {
                        *out = value_bool(!operand.as.bool_val);
                        return true;
                    }
                    break;
                case UNOP_BIT_NOT:
                    if (operand.type == VAL_INT) {
                        *out = value_int(~operand.as.int_val);
                        return true;
                    }
                    break;
            }
            value_free(&operand);
            return false;
        }

        case EXPR_BINOP: {
            /* Skip short-circuit operators */
            if (e->as.binop.op == BINOP_AND || e->as.binop.op == BINOP_OR || e->as.binop.op == BINOP_NIL_COALESCE)
                return false;

            LatValue lv, rv;
            if (!try_const_fold(e->as.binop.left, &lv)) return false;
            if (!try_const_fold(e->as.binop.right, &rv)) {
                value_free(&lv);
                return false;
            }

            /* String concatenation */
            if (lv.type == VAL_STR && rv.type == VAL_STR && e->as.binop.op == BINOP_ADD) {
                size_t llen = strlen(lv.as.str_val), rlen = strlen(rv.as.str_val);
                char *buf = malloc(llen + rlen + 1);
                memcpy(buf, lv.as.str_val, llen);
                memcpy(buf + llen, rv.as.str_val, rlen);
                buf[llen + rlen] = '\0';
                *out = value_string(buf);
                free(buf);
                value_free(&lv);
                value_free(&rv);
                return true;
            }

            /* Numeric operations */
            if ((lv.type != VAL_INT && lv.type != VAL_FLOAT) || (rv.type != VAL_INT && rv.type != VAL_FLOAT)) {
                value_free(&lv);
                value_free(&rv);
                return false;
            }

            bool both_int = (lv.type == VAL_INT && rv.type == VAL_INT);
            int64_t li = lv.type == VAL_INT ? lv.as.int_val : 0;
            int64_t ri = rv.type == VAL_INT ? rv.as.int_val : 0;
            double lf = lv.type == VAL_FLOAT ? lv.as.float_val : (double)li;
            double rf = rv.type == VAL_FLOAT ? rv.as.float_val : (double)ri;
            ValueNumericCmp cmp = value_numeric_compare(&lv, &rv);

            switch (e->as.binop.op) {
                case BINOP_ADD:
                    *out = both_int ? value_int((int64_t)((uint64_t)li + (uint64_t)ri)) : value_float(lf + rf);
                    return true;
                case BINOP_SUB:
                    *out = both_int ? value_int((int64_t)((uint64_t)li - (uint64_t)ri)) : value_float(lf - rf);
                    return true;
                case BINOP_MUL:
                    *out = both_int ? value_int((int64_t)((uint64_t)li * (uint64_t)ri)) : value_float(lf * rf);
                    return true;
                case BINOP_DIV:
                    if (both_int) {
                        if (ri == 0) return false;
                        *out = value_int(li == INT64_MIN && ri == -1 ? INT64_MIN : li / ri);
                    } else {
                        if (rf == 0.0) return false;
                        *out = value_float(lf / rf);
                    }
                    return true;
                case BINOP_MOD:
                    if (!both_int || ri == 0) return false;
                    *out = value_int(li == INT64_MIN && ri == -1 ? 0 : li % ri);
                    return true;
                case BINOP_LT: *out = value_bool(cmp == VALUE_CMP_LESS); return true;
                case BINOP_GT: *out = value_bool(cmp == VALUE_CMP_GREATER); return true;
                case BINOP_LTEQ: *out = value_bool(cmp == VALUE_CMP_LESS || cmp == VALUE_CMP_EQUAL); return true;
                case BINOP_GTEQ: *out = value_bool(cmp == VALUE_CMP_GREATER || cmp == VALUE_CMP_EQUAL); return true;
                case BINOP_EQ: *out = value_bool(cmp == VALUE_CMP_EQUAL); return true;
                case BINOP_NEQ: *out = value_bool(cmp != VALUE_CMP_EQUAL); return true;
                /* Bitwise: integer only */
                case BINOP_BIT_AND:
                    if (!both_int) return false;
                    *out = value_int(li & ri);
                    return true;
                case BINOP_BIT_OR:
                    if (!both_int) return false;
                    *out = value_int(li | ri);
                    return true;
                case BINOP_BIT_XOR:
                    if (!both_int) return false;
                    *out = value_int(li ^ ri);
                    return true;
                case BINOP_LSHIFT:
                    if (!both_int || ri < 0 || ri >= 64) return false;
                    *out = value_int((int64_t)((uint64_t)li << (unsigned)ri));
                    return true;
                case BINOP_RSHIFT:
                    if (!both_int || ri < 0 || ri >= 64) return false;
                    *out = value_int(li >> ri);
                    return true;
                default: return false;
            }
        }
        default: return false;
    }
}

/* Helper: emit a folded constant into register dst */
static void emit_folded_constant(LatValue folded, uint8_t dst, int line) {
    if (folded.type == VAL_INT && folded.as.int_val >= -32768 && folded.as.int_val <= 32767) {
        emit_AsBx(ROP_LOADI, dst, (int16_t)folded.as.int_val, line);
    } else if (folded.type == VAL_BOOL) {
        emit_ABC(folded.as.bool_val ? ROP_LOADTRUE : ROP_LOADFALSE, dst, 0, 0, line);
    } else {
        uint16_t ki = add_constant(folded);
        emit_ABx(ROP_LOADK, dst, ki, line);
    }
}

static void compile_expr(const Expr *e, uint8_t dst, int line) {
    if (rc_error) return;
    if (e->line > 0) line = e->line;

    /* LAT-545: capture-and-clear so only the outermost expression of a statement
     * observes the clean-context flag; every nested compile_expr sees false. */
    bool at_tos = g_reg_stmt_tos;
    g_reg_stmt_tos = false;

    switch (e->tag) {
        case EXPR_INT_LIT: {
            int64_t v = e->as.int_val;
            if (v >= -32768 && v <= 32767) {
                emit_AsBx(ROP_LOADI, dst, (int16_t)v, line);
            } else {
                uint16_t ki = add_constant(value_int(v));
                emit_ABx(ROP_LOADK, dst, ki, line);
            }
            break;
        }

        case EXPR_FLOAT_LIT: {
            uint16_t ki = add_constant(value_float(e->as.float_val));
            emit_ABx(ROP_LOADK, dst, ki, line);
            break;
        }

        case EXPR_STRING_LIT: {
            uint16_t ki = add_constant(value_string(e->as.str_val));
            emit_ABx(ROP_LOADK, dst, ki, line);
            break;
        }

        case EXPR_BOOL_LIT: emit_ABC(e->as.bool_val ? ROP_LOADTRUE : ROP_LOADFALSE, dst, 0, 0, line); break;

        case EXPR_NIL_LIT: emit_ABC(ROP_LOADNIL, dst, 0, 0, line); break;

        case EXPR_IDENT: {
            int local = resolve_local(rc, e->as.str_val);
            if (local >= 0) {
                uint8_t src = local_reg(local);
                if (src != dst) emit_ABC(ROP_MOVE, dst, src, 0, line);
            } else {
                int upvalue = resolve_upvalue(rc, e->as.str_val);
                if (upvalue >= 0) {
                    emit_ABC(ROP_GETUPVALUE, dst, (uint8_t)upvalue, 0, line);
                } else {
                    uint16_t ki = add_constant(value_string(e->as.str_val));
                    emit_ABx(ROP_GETGLOBAL, dst, ki, line);
                }
            }
            break;
        }

        case EXPR_BINOP: {
            /* Short-circuit AND/OR */
            if (e->as.binop.op == BINOP_AND) {
                compile_expr(e->as.binop.left, dst, line);
                size_t skip = emit_jump_placeholder(ROP_JMPFALSE, dst, line);
                compile_expr(e->as.binop.right, dst, line);
                patch_jump(skip);
                break;
            }
            if (e->as.binop.op == BINOP_OR) {
                compile_expr(e->as.binop.left, dst, line);
                size_t skip = emit_jump_placeholder(ROP_JMPTRUE, dst, line);
                compile_expr(e->as.binop.right, dst, line);
                patch_jump(skip);
                break;
            }

            /* Nil coalescing — use JMPNOTNIL for 2-instruction sequence */
            if (e->as.binop.op == BINOP_NIL_COALESCE) {
                compile_expr(e->as.binop.left, dst, line);
                size_t skip = emit_jump_placeholder(ROP_JMPNOTNIL, dst, line);
                compile_expr(e->as.binop.right, dst, line);
                patch_jump(skip);
                break;
            }

            /* Recursive constant folding */
            {
                LatValue folded;
                if (try_const_fold(e, &folded)) {
                    emit_folded_constant(folded, dst, line);
                    break;
                }
            }

            /* ADDI optimization: x + small_int or small_int + x */
            if (e->as.binop.op == BINOP_ADD) {
                if (e->as.binop.right->tag == EXPR_INT_LIT && e->as.binop.right->as.int_val >= -128 &&
                    e->as.binop.right->as.int_val <= 127) {
                    /* Compile left into dst, then ADDI */
                    compile_expr(e->as.binop.left, dst, line);
                    emit_ABC(ROP_ADDI, dst, dst, (uint8_t)(int8_t)e->as.binop.right->as.int_val, line);
                    break;
                }
                if (e->as.binop.left->tag == EXPR_INT_LIT && e->as.binop.left->as.int_val >= -128 &&
                    e->as.binop.left->as.int_val <= 127) {
                    compile_expr(e->as.binop.right, dst, line);
                    emit_ABC(ROP_ADDI, dst, dst, (uint8_t)(int8_t)e->as.binop.left->as.int_val, line);
                    break;
                }
            }

            /* General binary: compile left and right into separate temps to
             * avoid register aliasing when dst overlaps a source operand.
             * E.g. `y = x % y` where y is dst — compiling left into dst
             * would clobber y before the right operand reads it. */
            uint8_t lhs = alloc_reg();
            compile_expr(e->as.binop.left, lhs, line);
            uint8_t rhs = alloc_reg();
            compile_expr(e->as.binop.right, rhs, line);

            switch (e->as.binop.op) {
                case BINOP_ADD: emit_ABC(ROP_ADD, dst, lhs, rhs, line); break;
                case BINOP_SUB: emit_ABC(ROP_SUB, dst, lhs, rhs, line); break;
                case BINOP_MUL: emit_ABC(ROP_MUL, dst, lhs, rhs, line); break;
                case BINOP_DIV: emit_ABC(ROP_DIV, dst, lhs, rhs, line); break;
                case BINOP_MOD: emit_ABC(ROP_MOD, dst, lhs, rhs, line); break;
                case BINOP_EQ: emit_ABC(ROP_EQ, dst, lhs, rhs, line); break;
                case BINOP_NEQ: emit_ABC(ROP_NEQ, dst, lhs, rhs, line); break;
                case BINOP_LT: emit_ABC(ROP_LT, dst, lhs, rhs, line); break;
                case BINOP_GT: emit_ABC(ROP_GT, dst, lhs, rhs, line); break;
                case BINOP_LTEQ: emit_ABC(ROP_LTEQ, dst, lhs, rhs, line); break;
                case BINOP_GTEQ: emit_ABC(ROP_GTEQ, dst, lhs, rhs, line); break;
                case BINOP_BIT_AND: emit_ABC(ROP_BIT_AND, dst, lhs, rhs, line); break;
                case BINOP_BIT_OR: emit_ABC(ROP_BIT_OR, dst, lhs, rhs, line); break;
                case BINOP_BIT_XOR: emit_ABC(ROP_BIT_XOR, dst, lhs, rhs, line); break;
                case BINOP_LSHIFT: emit_ABC(ROP_LSHIFT, dst, lhs, rhs, line); break;
                case BINOP_RSHIFT: emit_ABC(ROP_RSHIFT, dst, lhs, rhs, line); break;
                default: rc_error = strdup("unsupported binary operator in regvm"); break;
            }
            free_reg(rhs);
            free_reg(lhs);
            break;
        }

        case EXPR_UNARYOP: {
            /* Recursive constant folding for unary expressions */
            LatValue folded;
            if (try_const_fold(e, &folded)) {
                emit_folded_constant(folded, dst, line);
                break;
            }
            compile_expr(e->as.unaryop.operand, dst, line);
            switch (e->as.unaryop.op) {
                case UNOP_NEG: emit_ABC(ROP_NEG, dst, dst, 0, line); break;
                case UNOP_NOT: emit_ABC(ROP_NOT, dst, dst, 0, line); break;
                case UNOP_BIT_NOT: emit_ABC(ROP_BIT_NOT, dst, dst, 0, line); break;
            }
            break;
        }

        case EXPR_IF: {
            compile_expr(e->as.if_expr.cond, dst, line);
            size_t else_jump = emit_jump_placeholder(ROP_JMPFALSE, dst, line);

            /* Then branch */
            begin_scope();
            if (e->as.if_expr.then_count > 0) {
                for (size_t i = 0; i + 1 < e->as.if_expr.then_count; i++) compile_stmt(e->as.if_expr.then_stmts[i]);
                Stmt *last = e->as.if_expr.then_stmts[e->as.if_expr.then_count - 1];
                if (last->tag == STMT_EXPR) compile_expr(last->as.expr, dst, line);
                else {
                    compile_stmt(last);
                    emit_ABC(ROP_LOADUNIT, dst, 0, 0, line);
                }
            } else {
                emit_ABC(ROP_LOADUNIT, dst, 0, 0, line);
            }
            end_scope(line);

            size_t end_jump = emit_jmp_placeholder(line);
            patch_jump(else_jump);

            /* Else branch */
            begin_scope();
            if (e->as.if_expr.else_count > 0) {
                for (size_t i = 0; i + 1 < e->as.if_expr.else_count; i++) compile_stmt(e->as.if_expr.else_stmts[i]);
                Stmt *last = e->as.if_expr.else_stmts[e->as.if_expr.else_count - 1];
                if (last->tag == STMT_EXPR) compile_expr(last->as.expr, dst, line);
                else {
                    compile_stmt(last);
                    emit_ABC(ROP_LOADUNIT, dst, 0, 0, line);
                }
            } else {
                emit_ABC(ROP_LOADUNIT, dst, 0, 0, line);
            }
            end_scope(line);
            patch_jmp(end_jump);
            break;
        }

        case EXPR_BLOCK: {
            begin_scope();
            if (e->as.block.count > 0) {
                for (size_t i = 0; i + 1 < e->as.block.count; i++) compile_stmt(e->as.block.stmts[i]);
                Stmt *last = e->as.block.stmts[e->as.block.count - 1];
                if (last->tag == STMT_EXPR) compile_expr(last->as.expr, dst, line);
                else {
                    compile_stmt(last);
                    emit_ABC(ROP_LOADUNIT, dst, 0, 0, line);
                }
            } else {
                emit_ABC(ROP_LOADUNIT, dst, 0, 0, line);
            }
            end_scope(line);
            break;
        }

        case EXPR_CALL: {
            /* Phase system special forms — intercept by function name */
            if (e->as.call.func->tag == EXPR_IDENT) {
                const char *fn = e->as.call.func->as.str_val;

                /* react(var, callback) → ROP_REACT */
                if (strcmp(fn, "react") == 0 && e->as.call.arg_count == 2 && e->as.call.args[0]->tag == EXPR_IDENT) {
                    uint16_t name_ki = add_constant(value_string(e->as.call.args[0]->as.str_val));
                    compile_expr(e->as.call.args[1], dst, line);
                    emit_ABx(ROP_REACT, dst, name_ki, line);
                    break;
                }
                /* unreact(var) → ROP_UNREACT */
                if (strcmp(fn, "unreact") == 0 && e->as.call.arg_count == 1 && e->as.call.args[0]->tag == EXPR_IDENT) {
                    uint16_t name_ki = add_constant(value_string(e->as.call.args[0]->as.str_val));
                    emit_ABx(ROP_UNREACT, dst, name_ki, line);
                    break;
                }
                /* bond(target, dep1, ..., [strategy]) → ROP_BOND */
                if (strcmp(fn, "bond") == 0 && e->as.call.arg_count >= 2 && e->as.call.args[0]->tag == EXPR_IDENT) {
                    const char *target = e->as.call.args[0]->as.str_val;
                    uint16_t target_ki = add_constant(value_string(target));
                    size_t dep_end = e->as.call.arg_count;
                    const char *strategy = "mirror";
                    Expr *last_arg = e->as.call.args[e->as.call.arg_count - 1];
                    if (last_arg->tag == EXPR_STRING_LIT) {
                        strategy = last_arg->as.str_val;
                        dep_end--;
                    }
                    for (size_t i = 1; i < dep_end; i++) {
                        const char *dep_name =
                            (e->as.call.args[i]->tag == EXPR_IDENT) ? e->as.call.args[i]->as.str_val : "";
                        uint8_t dep_reg = alloc_reg();
                        emit_ABx(ROP_LOADK, dep_reg, add_constant(value_string(dep_name)), line);
                        uint8_t strat_reg = alloc_reg();
                        emit_ABx(ROP_LOADK, strat_reg, add_constant(value_string(strategy)), line);
                        emit_ABC(ROP_BOND, (uint8_t)target_ki, dep_reg, strat_reg, line);
                        free_reg(strat_reg);
                        free_reg(dep_reg);
                    }
                    emit_ABC(ROP_LOADUNIT, dst, 0, 0, line);
                    break;
                }
                /* unbond(target, dep1, ...) → ROP_UNBOND */
                if (strcmp(fn, "unbond") == 0 && e->as.call.arg_count >= 2 && e->as.call.args[0]->tag == EXPR_IDENT) {
                    const char *target = e->as.call.args[0]->as.str_val;
                    uint16_t target_ki = add_constant(value_string(target));
                    for (size_t i = 1; i < e->as.call.arg_count; i++) {
                        const char *dep_name =
                            (e->as.call.args[i]->tag == EXPR_IDENT) ? e->as.call.args[i]->as.str_val : "";
                        uint8_t dep_reg = alloc_reg();
                        emit_ABx(ROP_LOADK, dep_reg, add_constant(value_string(dep_name)), line);
                        emit_ABx(ROP_UNBOND, (uint8_t)target_ki, (uint16_t)dep_reg, line);
                        free_reg(dep_reg);
                    }
                    emit_ABC(ROP_LOADUNIT, dst, 0, 0, line);
                    break;
                }
                /* seed(var, contract) → ROP_SEED */
                if (strcmp(fn, "seed") == 0 && e->as.call.arg_count == 2 && e->as.call.args[0]->tag == EXPR_IDENT) {
                    uint16_t name_ki = add_constant(value_string(e->as.call.args[0]->as.str_val));
                    compile_expr(e->as.call.args[1], dst, line);
                    emit_ABx(ROP_SEED, dst, name_ki, line);
                    break;
                }
                /* unseed(var) → ROP_UNSEED */
                if (strcmp(fn, "unseed") == 0 && e->as.call.arg_count == 1 && e->as.call.args[0]->tag == EXPR_IDENT) {
                    uint16_t name_ki = add_constant(value_string(e->as.call.args[0]->as.str_val));
                    emit_ABx(ROP_UNSEED, dst, name_ki, line);
                    break;
                }
                /* grow("varname") → FREEZE_VAR with consume=true (loc_type |= 0x80) */
                if (strcmp(fn, "grow") == 0 && e->as.call.arg_count == 1 &&
                    e->as.call.args[0]->tag == EXPR_STRING_LIT) {
                    const char *var_name = e->as.call.args[0]->as.str_val;
                    uint16_t name_ki = add_constant(value_string(var_name));
                    int slot = resolve_local(rc, var_name);
                    if (slot >= 0) {
                        emit_ABC(ROP_FREEZE_VAR, name_ki8(name_ki), 0 | 0x80, local_reg(slot), line);
                    } else {
                        int uv = resolve_upvalue(rc, var_name);
                        if (uv >= 0) {
                            emit_ABC(ROP_FREEZE_VAR, name_ki8(name_ki), 1 | 0x80, (uint8_t)uv, line);
                        } else {
                            emit_ABC(ROP_FREEZE_VAR, name_ki8(name_ki), 2 | 0x80, 0, line);
                        }
                    }
                    /* Result: load frozen value into dst */
                    if (slot >= 0) {
                        emit_ABC(ROP_MOVE, dst, local_reg(slot), 0, line);
                    } else {
                        int uv = resolve_upvalue(rc, var_name);
                        if (uv >= 0) emit_ABC(ROP_GETUPVALUE, dst, (uint8_t)uv, 0, line);
                        else emit_ABx(ROP_GETGLOBAL, dst, add_constant(value_string(var_name)), line);
                    }
                    break;
                }
                /* compose(f, g) → build a closure |x| { f(g(x)) } with upvalues */
                if (strcmp(fn, "compose") == 0 && e->as.call.arg_count == 2) {
                    /* Compile f and g into local registers */
                    uint8_t f_reg = alloc_reg();
                    uint8_t g_reg = alloc_reg();
                    compile_expr(e->as.call.args[0], f_reg, line);
                    compile_expr(e->as.call.args[1], g_reg, line);

                    /* Build a RegChunk for |x| { f(g(x)) } */
                    RegChunk *fn_chunk = regchunk_new();
                    fn_chunk->name = strdup("<compose>");
                    fn_chunk->max_reg = 5; /* uses R[0]..R[4] */
                    /* Constants: none needed (all via upvalues/registers) */
                    /* Registers: 0=reserved, 1=x (param), 2=temp */
                    /* Upvalue 0 = f, Upvalue 1 = g */

                    /* Emit: R[2] = GETUPVALUE 0 (f)
                     *       R[3] = GETUPVALUE 1 (g)
                     *       R[4] = R[1] (x)
                     *       CALL R[3], 1, 1 => g(x) -> R[3]
                     *       R[4] = R[3] (g_result)
                     *       CALL R[2], 1, 1 => f(g_result) -> R[2]
                     *       RETURN R[2], 1
                     */
                    regchunk_write(fn_chunk, REG_ENCODE_ABC(ROP_GETUPVALUE, 2, 0, 0), line); /* R[2] = f */
                    regchunk_write(fn_chunk, REG_ENCODE_ABC(ROP_GETUPVALUE, 3, 1, 0), line); /* R[3] = g */
                    regchunk_write(fn_chunk, REG_ENCODE_ABC(ROP_MOVE, 4, 1, 0), line);       /* R[4] = x */
                    regchunk_write(fn_chunk, REG_ENCODE_ABC(ROP_CALL, 3, 1, 1), line);       /* R[3] = g(x) */
                    regchunk_write(fn_chunk, REG_ENCODE_ABC(ROP_MOVE, 4, 3, 0), line);       /* R[4] = g_result */
                    regchunk_write(fn_chunk, REG_ENCODE_ABC(ROP_CALL, 2, 1, 1), line);       /* R[2] = f(g_result) */
                    regchunk_write(fn_chunk, REG_ENCODE_ABC(ROP_RETURN, 2, 1, 0), line);     /* return R[2] */

                    /* Store fn_chunk as a closure constant */
                    LatValue closure_val;
                    closure_val.type = VAL_CLOSURE;
                    closure_val.phase = VTAG_UNPHASED;
                    closure_val.region_id = REGION_NONE;
                    closure_val.as.closure.upvalue_count = 2;
                    closure_val.as.closure.body = NULL;
                    closure_val.as.closure.native_fn = (void *)fn_chunk;
                    closure_val.as.closure.captured_env = NULL;
                    closure_val.as.closure.param_count = 1;
                    closure_val.as.closure.param_names = NULL;
                    closure_val.as.closure.default_values = NULL;
                    closure_val.as.closure.has_variadic = false;

                    uint16_t closure_ki = add_constant(closure_val);

                    /* Emit CLOSURE instruction with upvalue capture directives */
                    emit_ABx(ROP_CLOSURE, dst, closure_ki, line);
                    /* Upvalue 0: f from local f_reg (is_local=1) */
                    emit(REG_ENCODE_ABC(0, 1, f_reg, 0), line);
                    /* Upvalue 1: g from local g_reg (is_local=1) */
                    emit(REG_ENCODE_ABC(0, 1, g_reg, 0), line);

                    /* Close upvalues before freeing registers — prevents aliasing */
                    emit_ABC(ROP_CLOSEUPVALUE, g_reg, 0, 0, line);
                    emit_ABC(ROP_CLOSEUPVALUE, f_reg, 0, 0, line);
                    free_reg(g_reg);
                    free_reg(f_reg);
                    break;
                }
                /* require("path") → ROP_REQUIRE (compiles module via regcompiler) */
                if (strcmp(fn, "require") == 0 && e->as.call.arg_count == 1 &&
                    e->as.call.args[0]->tag == EXPR_STRING_LIT) {
                    uint16_t path_ki = add_constant(value_string(e->as.call.args[0]->as.str_val));
                    emit_ABx(ROP_REQUIRE, dst, path_ki, line);
                    break;
                }
                /* track(var), history(var), phases(var): pass var name as string to native */
                if ((strcmp(fn, "track") == 0 || strcmp(fn, "history") == 0 || strcmp(fn, "phases") == 0) &&
                    e->as.call.arg_count == 1 && e->as.call.args[0]->tag == EXPR_IDENT) {
                    uint8_t base = alloc_reg();
                    compile_expr(e->as.call.func, base, line);
                    uint8_t arg_reg = alloc_reg();
                    emit_ABx(ROP_LOADK, arg_reg, add_constant(value_string(e->as.call.args[0]->as.str_val)), line);
                    emit_ABC(ROP_CALL, base, 1, 1, line);
                    if (base != dst) emit_ABC(ROP_MOVE, dst, base, 0, line);
                    free_regs_to(base);
                    break;
                }
                /* rewind(var, n): pass var name as string, compile second arg normally */
                if (strcmp(fn, "rewind") == 0 && e->as.call.arg_count == 2 && e->as.call.args[0]->tag == EXPR_IDENT) {
                    uint8_t base = alloc_reg();
                    compile_expr(e->as.call.func, base, line);
                    uint8_t arg1 = alloc_reg();
                    emit_ABx(ROP_LOADK, arg1, add_constant(value_string(e->as.call.args[0]->as.str_val)), line);
                    uint8_t arg2 = alloc_reg();
                    compile_expr(e->as.call.args[1], arg2, line);
                    emit_ABC(ROP_CALL, base, 2, 1, line);
                    if (base != dst) emit_ABC(ROP_MOVE, dst, base, 0, line);
                    free_regs_to(base);
                    break;
                }
                /* pressurize(var, mode): pass var name as string */
                if (strcmp(fn, "pressurize") == 0 && e->as.call.arg_count == 2 &&
                    e->as.call.args[0]->tag == EXPR_IDENT) {
                    uint8_t base = alloc_reg();
                    compile_expr(e->as.call.func, base, line);
                    uint8_t arg1 = alloc_reg();
                    emit_ABx(ROP_LOADK, arg1, add_constant(value_string(e->as.call.args[0]->as.str_val)), line);
                    uint8_t arg2 = alloc_reg();
                    compile_expr(e->as.call.args[1], arg2, line);
                    emit_ABC(ROP_CALL, base, 2, 1, line);
                    if (base != dst) emit_ABC(ROP_MOVE, dst, base, 0, line);
                    free_regs_to(base);
                    break;
                }
                /* depressurize(var): pass var name as string */
                if (strcmp(fn, "depressurize") == 0 && e->as.call.arg_count == 1 &&
                    e->as.call.args[0]->tag == EXPR_IDENT) {
                    uint8_t base = alloc_reg();
                    compile_expr(e->as.call.func, base, line);
                    uint8_t arg1 = alloc_reg();
                    emit_ABx(ROP_LOADK, arg1, add_constant(value_string(e->as.call.args[0]->as.str_val)), line);
                    emit_ABC(ROP_CALL, base, 1, 1, line);
                    if (base != dst) emit_ABC(ROP_MOVE, dst, base, 0, line);
                    free_regs_to(base);
                    break;
                }
            }

            /* General function call */
            uint8_t base = alloc_reg();
            compile_expr(e->as.call.func, base, line);
            for (size_t i = 0; i < e->as.call.arg_count; i++) {
                uint8_t arg_reg = alloc_reg();
                compile_expr(e->as.call.args[i], arg_reg, line);
            }
            /* CALL: A=base (func reg), B=arg count, C=1 (one return value) */
            emit_ABC(ROP_CALL, base, (uint8_t)e->as.call.arg_count, 1, line);
            /* Result lands in base. Move to dst if needed. */
            if (base != dst) emit_ABC(ROP_MOVE, dst, base, 0, line);
            /* Free the window (func + args) */
            free_regs_to(base);
            break;
        }

        case EXPR_FIELD_ACCESS: {
            /* Compile object, then GETFIELD */
            uint8_t obj_reg;
            bool obj_reg_temp = false;

            if (e->as.field_access.optional) {
                /* Optional chaining: need separate obj_reg to avoid LOADNIL clobbering */
                obj_reg = alloc_reg();
                obj_reg_temp = true;
                compile_expr(e->as.field_access.object, obj_reg, line);
            } else if (e->as.field_access.object->tag == EXPR_IDENT) {
                int local = resolve_local(rc, e->as.field_access.object->as.str_val);
                if (local >= 0) {
                    obj_reg = local_reg(local);
                } else {
                    obj_reg = dst;
                    compile_expr(e->as.field_access.object, obj_reg, line);
                }
            } else {
                obj_reg = dst;
                compile_expr(e->as.field_access.object, obj_reg, line);
            }

            /* Optional chaining: obj?.field → if obj is nil, result is nil */
            if (e->as.field_access.optional) {
                /* If obj is nil, skip the field access and leave nil in dst */
                emit_ABC(ROP_LOADNIL, dst, 0, 0, line);
                size_t skip = emit_jump_placeholder(ROP_JMPFALSE, obj_reg, line);
                uint16_t field_ki = add_constant(value_string(e->as.field_access.field));
                emit_getfield(dst, obj_reg, field_ki, line);
                patch_jump(skip);
                if (obj_reg_temp) free_reg(obj_reg);
            } else {
                uint16_t field_ki = add_constant(value_string(e->as.field_access.field));
                emit_getfield(dst, obj_reg, field_ki, line);
            }
            break;
        }

        case EXPR_INDEX: {
            uint8_t obj_reg;
            bool obj_allocated = false;

            if (e->as.index.object->tag == EXPR_IDENT) {
                int local = resolve_local(rc, e->as.index.object->as.str_val);
                if (local >= 0) {
                    obj_reg = local_reg(local);
                } else {
                    obj_reg = alloc_reg();
                    obj_allocated = true;
                    compile_expr(e->as.index.object, obj_reg, line);
                }
            } else {
                obj_reg = alloc_reg();
                obj_allocated = true;
                compile_expr(e->as.index.object, obj_reg, line);
            }

            /* Optional chaining: obj?[idx] → if obj is nil, result is nil */
            if (e->as.index.optional) {
                uint8_t check = alloc_reg();
                emit_ABC(ROP_MOVE, check, obj_reg, 0, line);
                emit_ABC(ROP_LOADNIL, dst, 0, 0, line);
                size_t skip = emit_jump_placeholder(ROP_JMPFALSE, check, line);
                free_reg(check);
                uint8_t idx_reg = alloc_reg();
                compile_expr(e->as.index.index, idx_reg, line);
                emit_ABC(ROP_GETINDEX, dst, obj_reg, idx_reg, line);
                free_reg(idx_reg);
                patch_jump(skip);
            } else {
                uint8_t idx_reg = alloc_reg();
                compile_expr(e->as.index.index, idx_reg, line);
                emit_ABC(ROP_GETINDEX, dst, obj_reg, idx_reg, line);
                free_reg(idx_reg);
            }
            if (obj_allocated) free_reg(obj_reg);
            break;
        }

        case EXPR_ARRAY: {
            /* Check for spread elements */
            bool has_spread = false;
            for (size_t i = 0; i < e->as.array.count; i++) {
                if (e->as.array.elems[i]->tag == EXPR_SPREAD) {
                    has_spread = true;
                    break;
                }
            }
            /* Compile elements into contiguous registers starting at dst */
            uint8_t base = alloc_reg();
            for (size_t i = 0; i < e->as.array.count; i++) {
                uint8_t elem_reg = (i == 0) ? base : alloc_reg();
                compile_expr(e->as.array.elems[i], elem_reg, line);
            }
            if (e->as.array.count == 0) {
                /* Empty array: B=base doesn't matter, C=0 */
                emit_ABC(ROP_NEWARRAY, dst, 0, 0, line);
            } else {
                emit_ABC(ROP_NEWARRAY, dst, base, (uint8_t)e->as.array.count, line);
            }
            if (has_spread) emit_ABC(ROP_ARRAY_FLATTEN, dst, dst, 0, line);
            free_regs_to(base);
            break;
        }

        case EXPR_STRUCT_LIT: {
            /* Compile field values into contiguous registers, then NEWSTRUCT */
            uint16_t name_ki = add_constant(value_string(e->as.struct_lit.name));

            /* We need the struct metadata to know field order */
            uint8_t base = alloc_reg();
            for (size_t i = 0; i < e->as.struct_lit.field_count; i++) {
                uint8_t freg = (i == 0) ? base : alloc_reg();
                compile_expr(e->as.struct_lit.fields[i].value, freg, line);
            }
            emit_ABC(ROP_NEWSTRUCT, dst, (uint8_t)(name_ki & 0xFF), (uint8_t)e->as.struct_lit.field_count, line);
            /* Store the name constant index in a follow-up instruction for the VM */
            emit_ABx(ROP_LOADK, base, name_ki, line); /* Overloaded: VM reads this as struct name */
            /* Preserve source field names as data words. RegVM used to pair
             * source-order values with declaration-order metadata, so a literal
             * such as P{y: 2, x: 1} silently swapped the two values. */
            for (size_t i = 0; i < e->as.struct_lit.field_count; i++) {
                uint16_t field_ki = add_constant(value_string(e->as.struct_lit.fields[i].name));
                emit_ABx(ROP_LOADK, 0, field_ki, line);
            }
            free_regs_to(base);
            break;
        }

        case EXPR_RANGE: {
            uint8_t start_reg = alloc_reg();
            uint8_t end_reg = alloc_reg();
            compile_expr(e->as.range.start, start_reg, line);
            compile_expr(e->as.range.end, end_reg, line);
            emit_ABC(ROP_BUILDRANGE, dst, start_reg, end_reg, line);
            free_reg(end_reg);
            free_reg(start_reg);
            break;
        }

        case EXPR_PRINT: {
            /* Compile args into contiguous registers */
            uint8_t base = alloc_reg();
            for (size_t i = 0; i < e->as.print.arg_count; i++) {
                uint8_t preg = (i == 0) ? base : alloc_reg();
                compile_expr(e->as.print.args[i], preg, line);
            }
            emit_ABC(ROP_PRINT, base, (uint8_t)e->as.print.arg_count, 0, line);
            emit_ABC(ROP_LOADUNIT, dst, 0, 0, line);
            free_regs_to(base);
            break;
        }

        case EXPR_FREEZE: {
            /* ── Partial freeze: freeze(s.field) for struct fields ── */
            if (e->as.freeze.except_count == 0 && !e->as.freeze.contract &&
                e->as.freeze.expr->tag == EXPR_FIELD_ACCESS) {
                Expr *parent = e->as.freeze.expr->as.field_access.object;
                const char *field = e->as.freeze.expr->as.field_access.field;
                if (parent->tag == EXPR_IDENT) {
                    const char *pname = parent->as.str_val;
                    int slot = resolve_local(rc, pname);
                    uint8_t var_reg = alloc_reg();
                    if (slot >= 0) {
                        emit_ABC(ROP_MOVE, var_reg, local_reg(slot), 0, line);
                    } else {
                        int uv = resolve_upvalue(rc, pname);
                        uint16_t pki = add_constant(value_string(pname));
                        if (uv >= 0) emit_ABC(ROP_GETUPVALUE, var_reg, (uint8_t)uv, 0, line);
                        else emit_ABx(ROP_GETGLOBAL, var_reg, pki, line);
                    }
                    uint16_t fk = add_constant(value_string(field));
                    emit_ABC(ROP_FREEZE_FIELD, var_reg, name_ki8(fk), 0, line);
                    /* Write back */
                    if (slot >= 0) {
                        emit_ABC(ROP_MOVE, local_reg(slot), var_reg, 0, line);
                    } else {
                        int uv2 = resolve_upvalue(rc, pname);
                        uint16_t pki2 = add_constant(value_string(pname));
                        if (uv2 >= 0) emit_ABC(ROP_SETUPVALUE, var_reg, (uint8_t)uv2, 0, line);
                        else emit_ABx(ROP_SETGLOBAL, var_reg, pki2, line);
                    }
                    /* Return the frozen field value */
                    emit_getfield(dst, var_reg, fk, line);
                    free_reg(var_reg);
                    break;
                }
            }

            /* ── Partial freeze: freeze(m["key"]) for map keys ── */
            if (e->as.freeze.except_count == 0 && !e->as.freeze.contract && e->as.freeze.expr->tag == EXPR_INDEX) {
                Expr *parent = e->as.freeze.expr->as.index.object;
                if (parent->tag == EXPR_IDENT) {
                    const char *pname = parent->as.str_val;
                    uint8_t key_reg = alloc_reg();
                    compile_expr(e->as.freeze.expr->as.index.index, key_reg, line);
                    int slot = resolve_local(rc, pname);
                    uint8_t var_reg = alloc_reg();
                    if (slot >= 0) {
                        emit_ABC(ROP_MOVE, var_reg, local_reg(slot), 0, line);
                    } else {
                        int uv = resolve_upvalue(rc, pname);
                        uint16_t pki = add_constant(value_string(pname));
                        if (uv >= 0) emit_ABC(ROP_GETUPVALUE, var_reg, (uint8_t)uv, 0, line);
                        else emit_ABx(ROP_GETGLOBAL, var_reg, pki, line);
                    }
                    /* For FREEZE_FIELD with map, the key is in a register but opcode takes constant.
                     * Since m["key"] has a string literal key, extract it. */
                    Expr *idx_expr = e->as.freeze.expr->as.index.index;
                    if (idx_expr->tag == EXPR_STRING_LIT) {
                        uint16_t fk = add_constant(value_string(idx_expr->as.str_val));
                        emit_ABC(ROP_FREEZE_FIELD, var_reg, name_ki8(fk), 0, line);
                    }
                    /* Write back */
                    if (slot >= 0) {
                        emit_ABC(ROP_MOVE, local_reg(slot), var_reg, 0, line);
                    } else {
                        int uv2 = resolve_upvalue(rc, pname);
                        uint16_t pki2 = add_constant(value_string(pname));
                        if (uv2 >= 0) emit_ABC(ROP_SETUPVALUE, var_reg, (uint8_t)uv2, 0, line);
                        else emit_ABx(ROP_SETGLOBAL, var_reg, pki2, line);
                    }
                    /* Return the frozen value */
                    emit_ABC(ROP_GETINDEX, dst, var_reg, key_reg, line);
                    free_reg(key_reg);
                    free_reg(var_reg);
                    break;
                }
            }

            compile_expr(e->as.freeze.expr, dst, line);

            /* Freeze-except: freeze(x) except ["field1", ...] */
            if (e->as.freeze.except_count > 0 && e->as.freeze.expr->tag == EXPR_IDENT) {
                /* Compile except field names into contiguous temporary registers */
                uint8_t except_base = alloc_reg();
                for (size_t i = 0; i < e->as.freeze.except_count; i++) {
                    uint8_t ereg = (i == 0) ? except_base : alloc_reg();
                    compile_expr(e->as.freeze.except_fields[i], ereg, line);
                }
                /* Emit ROP_FREEZE_EXCEPT two-instruction sequence:
                 *   FREEZE_EXCEPT name_ki, loc_type, slot
                 *   data:         except_base, except_count, 0 */
                const char *name = e->as.freeze.expr->as.str_val;
                uint16_t name_ki = add_constant(value_string(name));
                int slot = resolve_local(rc, name);
                uint8_t loc_type, slot_val;
                if (slot >= 0) {
                    loc_type = 0;
                    slot_val = local_reg(slot);
                } else {
                    int uv = resolve_upvalue(rc, name);
                    if (uv >= 0) {
                        loc_type = 1;
                        slot_val = (uint8_t)uv;
                    } else {
                        loc_type = 2;
                        slot_val = 0;
                    }
                }
                emit_ABC(ROP_FREEZE_EXCEPT, name_ki8(name_ki), loc_type, slot_val, line);
                emit_ABC(ROP_MOVE, except_base, (uint8_t)e->as.freeze.except_count, 0, line);
                /* Free except field registers */
                if (e->as.freeze.except_count == 0) free_reg(except_base);
                else free_regs_to(except_base);
                break;
            }

            /* Freeze contract: freeze(x) where |v| { ... }
             * The contract closure is called with the value; errors are wrapped
             * with "freeze contract failed: " prefix. */
            if (e->as.freeze.contract && e->as.freeze.expr->tag == EXPR_IDENT) {
                /* Save the value in a temp before handler setup (handler may clobber regs) */
                uint8_t saved_val = alloc_reg();
                emit_ABC(ROP_MOVE, saved_val, dst, 0, line);

                uint8_t err_reg = alloc_reg();
                size_t handler_jump = emit_jump_placeholder(ROP_PUSH_HANDLER, err_reg, line);
                rc->handler_depth++;

                /* Compile contract closure and call it with the saved value */
                uint8_t base = alloc_reg();
                uint8_t arg = alloc_reg();
                compile_expr(e->as.freeze.contract, base, line);
                emit_ABC(ROP_MOVE, arg, saved_val, 0, line);
                emit_ABC(ROP_CALL, base, 1, 1, line);
                free_reg(arg);
                free_reg(base);

                /* Success — pop handler, jump past catch */
                emit_ABC(ROP_POP_HANDLER, 0, 0, 0, line);
                rc->handler_depth--;
                size_t skip = emit_jmp_placeholder(line);

                /* Catch: err_reg has error string */
                patch_jump(handler_jump);
                {
                    uint8_t pfx = alloc_reg();
                    uint16_t pfx_ki = add_constant(value_string("freeze contract failed: "));
                    emit_ABx(ROP_LOADK, pfx, pfx_ki, line);
                    emit_ABC(ROP_CONCAT, err_reg, pfx, err_reg, line);
                    free_reg(pfx);
                }
                emit_ABC(ROP_THROW, err_reg, 0, 0, line);

                patch_jmp(skip);
                /* Restore dst from saved value (CALL may have clobbered dst) */
                emit_ABC(ROP_MOVE, dst, saved_val, 0, line);
                free_reg(err_reg);
                free_reg(saved_val);
            }

            /* Normal freeze (or after contract passed) */
            if (e->as.freeze.expr->tag == EXPR_IDENT) {
                const char *name = e->as.freeze.expr->as.str_val;
                uint16_t name_ki = add_constant(value_string(name));
                int slot = resolve_local(rc, name);
                if (slot >= 0) {
                    emit_ABC(ROP_FREEZE_VAR, name_ki8(name_ki), 0, local_reg(slot), line);
                } else {
                    int uv = resolve_upvalue(rc, name);
                    if (uv >= 0) {
                        emit_ABC(ROP_FREEZE_VAR, name_ki8(name_ki), 1, (uint8_t)uv, line);
                    } else {
                        emit_ABC(ROP_FREEZE_VAR, name_ki8(name_ki), 2, 0, line);
                    }
                }
                /* FREEZE_VAR writes back to the variable but dst still holds the
                   stale (unfrozen) copy — freeze it so the expression result is correct. */
                emit_ABC(ROP_FREEZE, dst, dst, 0, line);
            } else if (e->as.freeze.expr->tag == EXPR_FIELD_ACCESS) {
                /* Partial freeze: freeze(s.field) */
                Expr *parent = e->as.freeze.expr->as.field_access.object;
                const char *field = e->as.freeze.expr->as.field_access.field;
                if (parent->tag == EXPR_IDENT) {
                    const char *pname = parent->as.str_val;
                    uint16_t pname_ki = add_constant(value_string(pname));
                    uint16_t field_ki = add_constant(value_string(field));
                    int slot = resolve_local(rc, pname);
                    uint8_t loc_type, slot_val;
                    if (slot >= 0) {
                        loc_type = 0;
                        slot_val = local_reg(slot);
                    } else {
                        int uv = resolve_upvalue(rc, pname);
                        if (uv >= 0) {
                            loc_type = 1;
                            slot_val = (uint8_t)uv;
                        } else {
                            loc_type = 2;
                            slot_val = 0;
                        }
                    }
                    /* Emit a FREEZE_VAR with field_ki encoded — but we don't have an opcode for that.
                     * Instead, freeze the value in the register and write back. */
                    emit_ABC(ROP_FREEZE, dst, dst, 0, line);
                    /* Write the frozen field back: parent.field = frozen_dst */
                    uint8_t parent_reg = alloc_reg();
                    if (loc_type == 0) emit_ABC(ROP_MOVE, parent_reg, slot_val, 0, line);
                    else if (loc_type == 1) emit_ABC(ROP_GETUPVALUE, parent_reg, slot_val, 0, line);
                    else emit_ABx(ROP_GETGLOBAL, parent_reg, pname_ki, line);
                    emit_setfield(parent_reg, field_ki, dst, line);
                    /* Write back the modified parent */
                    if (loc_type == 0) emit_ABC(ROP_MOVE, slot_val, parent_reg, 0, line);
                    else if (loc_type == 1) emit_ABC(ROP_SETUPVALUE, parent_reg, slot_val, 0, line);
                    else emit_ABx(ROP_SETGLOBAL, parent_reg, pname_ki, line);
                    free_reg(parent_reg);
                } else {
                    emit_ABC(ROP_FREEZE, dst, dst, 0, line);
                }
            } else if (e->as.freeze.expr->tag == EXPR_INDEX) {
                /* Partial freeze: freeze(m["key"]) */
                Expr *parent = e->as.freeze.expr->as.index.object;
                if (parent->tag == EXPR_IDENT) {
                    const char *pname = parent->as.str_val;
                    uint16_t pname_ki = add_constant(value_string(pname));
                    int slot = resolve_local(rc, pname);
                    uint8_t loc_type, slot_val;
                    if (slot >= 0) {
                        loc_type = 0;
                        slot_val = local_reg(slot);
                    } else {
                        int uv = resolve_upvalue(rc, pname);
                        if (uv >= 0) {
                            loc_type = 1;
                            slot_val = (uint8_t)uv;
                        } else {
                            loc_type = 2;
                            slot_val = 0;
                        }
                    }
                    /* Freeze the indexed value in-register */
                    emit_ABC(ROP_FREEZE, dst, dst, 0, line);
                    /* Write back: parent[key] = frozen */
                    uint8_t parent_reg = alloc_reg();
                    uint8_t key_reg = alloc_reg();
                    if (loc_type == 0) emit_ABC(ROP_MOVE, parent_reg, slot_val, 0, line);
                    else if (loc_type == 1) emit_ABC(ROP_GETUPVALUE, parent_reg, slot_val, 0, line);
                    else emit_ABx(ROP_GETGLOBAL, parent_reg, pname_ki, line);
                    compile_expr(e->as.freeze.expr->as.index.index, key_reg, line);
                    emit_ABC(ROP_SETINDEX, parent_reg, key_reg, dst, line);
                    if (loc_type == 0) emit_ABC(ROP_MOVE, slot_val, parent_reg, 0, line);
                    else if (loc_type == 1) emit_ABC(ROP_SETUPVALUE, parent_reg, slot_val, 0, line);
                    else emit_ABx(ROP_SETGLOBAL, parent_reg, pname_ki, line);
                    free_reg(key_reg);
                    free_reg(parent_reg);
                } else {
                    emit_ABC(ROP_FREEZE, dst, dst, 0, line);
                }
            } else {
                emit_ABC(ROP_FREEZE, dst, dst, 0, line);
            }
            break;
        }

        case EXPR_THAW: {
            compile_expr(e->as.freeze_expr, dst, line);
            if (e->as.freeze_expr->tag == EXPR_IDENT) {
                const char *name = e->as.freeze_expr->as.str_val;
                uint16_t name_ki = add_constant(value_string(name));
                int slot = resolve_local(rc, name);
                if (slot >= 0) {
                    emit_ABC(ROP_THAW_VAR, name_ki8(name_ki), 0, local_reg(slot), line);
                } else {
                    int uv = resolve_upvalue(rc, name);
                    if (uv >= 0) {
                        emit_ABC(ROP_THAW_VAR, name_ki8(name_ki), 1, (uint8_t)uv, line);
                    } else {
                        emit_ABC(ROP_THAW_VAR, name_ki8(name_ki), 2, 0, line);
                    }
                }
                /* THAW_VAR writes back to the variable but dst still holds the
                   stale (frozen) copy — thaw it so the expression result is correct. */
                emit_ABC(ROP_THAW, dst, dst, 0, line);
            } else {
                emit_ABC(ROP_THAW, dst, dst, 0, line);
            }
            break;
        }

        case EXPR_CLONE: {
            compile_expr(e->as.freeze_expr, dst, line);
            emit_ABC(ROP_CLONE, dst, dst, 0, line);
            break;
        }

        case EXPR_METHOD_CALL: {
            /* LAT-545: for a nested-element mutating call through a chain with a
             * side-effecting index, evaluate each impure index level once (into a
             * held register) so the optional nil-check, the receiver read, and the
             * write-back all reuse it. Hoisted at the top (before the optional
             * nil-check) so the nil-check does not re-evaluate the index. Gated on
             * at_tos to mirror the stack VM (which needs a clean stack). When
             * hoisting, the object is an EXPR_INDEX, so the IDENT global/local fast
             * paths below are never taken and control always reaches the matching
             * reg_idx_hoist_end in the expression-object path. */
            Expr *mc_obj = e->as.method_call.object;
            bool mc_hoist =
                at_tos && mc_obj->tag == EXPR_INDEX && index_chain_writable(mc_obj) && chain_has_impure_index(mc_obj);
            RegIdxHoist mc_h;
            if (mc_hoist) reg_idx_hoist_begin(&mc_h, mc_obj, line);

            /* Optional chaining: obj?.method() — if obj is nil, return nil */
            size_t opt_skip = 0;
            if (e->as.method_call.optional) {
                uint8_t check = alloc_reg();
                compile_expr(e->as.method_call.object, check, line);
                emit_ABC(ROP_LOADNIL, dst, 0, 0, line);
                /* JMPNOTNIL: if check is NOT nil, skip the early return (jump over) */
                /* We want: if nil, skip to end. So use JMPNOTNIL to skip the jump. */
                size_t not_nil = emit_jump_placeholder(ROP_JMPNOTNIL, check, line);
                /* Object is nil — jump to end */
                opt_skip = emit_jmp_placeholder(line);
                patch_jump(not_nil);
                free_reg(check);
            }

            /* Check if the object is a global variable — needs INVOKE_GLOBAL for
             * in-place mutation (push/pop/etc) to persist on the global. */
            bool is_global = false;
            const char *global_name = NULL;
            if (e->as.method_call.object->tag == EXPR_IDENT) {
                int local = resolve_local(rc, e->as.method_call.object->as.str_val);
                if (local < 0) {
                    int uv = resolve_upvalue(rc, e->as.method_call.object->as.str_val);
                    if (uv < 0) {
                        is_global = true;
                        global_name = e->as.method_call.object->as.str_val;
                    }
                }
            }

            if (is_global) {
                /* INVOKE_GLOBAL two-instruction sequence:
                 *   INVOKE_GLOBAL dst, name_ki, argc
                 *   data:         method_ki, args_base, 0
                 * VM gets env_get_ref() for in-place mutation. */
                uint8_t args_base = alloc_reg();
                for (size_t i = 0; i < e->as.method_call.arg_count; i++) {
                    uint8_t arg_reg = (i == 0) ? args_base : alloc_reg();
                    compile_expr(e->as.method_call.args[i], arg_reg, line);
                }

                uint16_t name_ki = add_constant(value_string(global_name));
                uint16_t method_ki = add_constant(value_string(e->as.method_call.method));

                /* LAT-463: the global name has no spare bits (8-bit operand) so
                 * refuse if it overflows; the method key carries a hi byte in
                 * the data word's C field, giving it a full 16-bit index. */
                emit_ABC(ROP_INVOKE_GLOBAL, dst, name_ki8(name_ki), (uint8_t)e->as.method_call.arg_count, line);
                emit_ABC(ROP_MOVE, (uint8_t)(method_ki & 0xFF), args_base, (uint8_t)(method_ki >> 8), line);

                if (e->as.method_call.arg_count == 0) free_reg(args_base);
                else free_regs_to(args_base);
                if (opt_skip) patch_jmp(opt_skip);
                break;
            }

            /* Check if the object is a local variable — use INVOKE_LOCAL for
             * direct in-place mutation (push/pop/etc) on the local register. */
            if (e->as.method_call.object->tag == EXPR_IDENT) {
                int local = resolve_local(rc, e->as.method_call.object->as.str_val);
                if (local >= 0) {
                    /* INVOKE_LOCAL two-instruction sequence:
                     *   INVOKE_LOCAL dst, local_reg, argc
                     *   data:        method_ki, args_base, 0
                     * VM mutates R[local_reg] in-place. */
                    uint8_t args_base = alloc_reg();
                    for (size_t i = 0; i < e->as.method_call.arg_count; i++) {
                        uint8_t arg_reg = (i == 0) ? args_base : alloc_reg();
                        compile_expr(e->as.method_call.args[i], arg_reg, line);
                    }

                    uint16_t method_ki = add_constant(value_string(e->as.method_call.method));

                    /* LAT-463: method key is 16-bit — lo in data-word A, hi in
                     * data-word C (previously an unused/zero field). */
                    emit_ABC(ROP_INVOKE_LOCAL, dst, local_reg(local), (uint8_t)e->as.method_call.arg_count, line);
                    emit_ABC(ROP_MOVE, (uint8_t)(method_ki & 0xFF), args_base, (uint8_t)(method_ki >> 8), line);

                    if (e->as.method_call.arg_count == 0) free_reg(args_base);
                    else free_regs_to(args_base);
                    if (opt_skip) patch_jmp(opt_skip);
                    break;
                }
            }

            /* Upvalue or expression object: two-instruction INVOKE sequence:
             *   INVOKE dst, method_ki, argc
             *   data:  obj_reg, args_base, 0
             * Object is compiled into a temp register; mutation goes to that temp. */
            uint8_t obj_reg;
            bool obj_allocated = false;

            if (e->as.method_call.object->tag == EXPR_IDENT) {
                /* Upvalue — compile expression to temp reg */
                obj_reg = alloc_reg();
                obj_allocated = true;
                compile_expr(e->as.method_call.object, obj_reg, line);
            } else {
                obj_reg = alloc_reg();
                obj_allocated = true;
                compile_expr(e->as.method_call.object, obj_reg, line);
            }

            /* Compile args into contiguous temp registers */
            uint8_t args_base = alloc_reg(); /* placeholder even if 0 args */
            for (size_t i = 0; i < e->as.method_call.arg_count; i++) {
                uint8_t arg_reg = (i == 0) ? args_base : alloc_reg();
                compile_expr(e->as.method_call.args[i], arg_reg, line);
            }

            uint16_t method_ki = add_constant(value_string(e->as.method_call.method));

            if (e->as.method_call.object->tag == EXPR_INDEX && index_chain_writable(e->as.method_call.object)) {
                /* LAT-544: nested-element mutating method call. The general
                 * INVOKE path clones obj[i] into obj_reg via GETINDEX, mutates
                 * the clone, and discards it — so obj[i].push(x) silently
                 * no-ops. Emit a 3-word ROP_INVOKE_MUT followed immediately by
                 * the index write-back chain; word2 (`skip`) records the chain's
                 * word count. At runtime the chain runs only when the call was a
                 * real in-place builtin mutation (push/pop/...); otherwise the VM
                 * advances past it (impl Type::method, struct/map closure field,
                 * non-mutating builtin, missing method — including frozen
                 * receivers, which must NOT trigger a write-back). */
                emit_ABC(ROP_INVOKE_MUT, dst, (uint8_t)(method_ki & 0xFF), (uint8_t)e->as.method_call.arg_count, line);
                emit_ABC(ROP_MOVE, obj_reg, args_base, (uint8_t)(method_ki >> 8),
                         line);                                /* data word (not executed as MOVE) */
                size_t skip_word = emit_ABC(0, 0, 0, 0, line); /* word2: skip placeholder (raw u32) */
                size_t wb_anchor = current_chunk()->code_len;

                /* Free the args first so the write-back's temporaries reuse the
                 * arg slots, never obj_reg; obj_reg must survive the write-back. */
                if (e->as.method_call.arg_count == 0) free_reg(args_base);
                else free_regs_to(args_base);

                emit_index_writeback(e->as.method_call.object, obj_reg, line);

                /* Patch the raw forward skip = words emitted for the chain. */
                if (!rc_error) {
                    size_t skip = current_chunk()->code_len - wb_anchor;
                    current_chunk()->code[skip_word] = (RegInstr)skip;
                }
                if (obj_allocated) free_reg(obj_reg); /* free obj_reg LAST */
            } else {
                /* LAT-463: method key is 16-bit — lo in word1 B, hi in the data
                 * word's C field (previously an unused/zero field). */
                emit_ABC(ROP_INVOKE, dst, (uint8_t)(method_ki & 0xFF), (uint8_t)e->as.method_call.arg_count, line);
                emit_ABC(ROP_MOVE, obj_reg, args_base, (uint8_t)(method_ki >> 8),
                         line); /* data word (not executed as MOVE) */

                /* Free temp registers */
                if (e->as.method_call.arg_count == 0) free_reg(args_base);
                else free_regs_to(args_base);
                if (obj_allocated) free_reg(obj_reg);
            }
            if (mc_hoist) reg_idx_hoist_end(&mc_h);
            if (opt_skip) patch_jmp(opt_skip);
            break;
        }

        case EXPR_CLOSURE: {
            RegCompiler func_comp;
            rc_init(&func_comp, rc, REG_FUNC_CLOSURE);

            /* Count non-variadic params for arity */
            size_t declared_arity = e->as.closure.param_count;
            if (e->as.closure.has_variadic && declared_arity > 0) declared_arity--;
            func_comp.arity = (int)declared_arity;

            for (size_t i = 0; i < e->as.closure.param_count; i++) add_local(e->as.closure.params[i]);

            /* Emit default parameter initialization */
            int rc_default_count = 0;
            if (e->as.closure.default_values) {
                for (size_t i = 0; i < e->as.closure.param_count; i++) {
                    if (e->as.closure.default_values[i]) {
                        rc_default_count++;
                        uint8_t preg = local_reg((int)(i + 1));
                        size_t skip = emit_default_skip(preg, line);
                        compile_expr(e->as.closure.default_values[i], preg, line);
                        patch_jump(skip);
                    }
                }
            }
            /* LAT-451: record default count on the chunk so callable-struct-field
             * arity checks can distinguish required from optional params. */
            func_comp.chunk->default_count = rc_default_count;

            /* Emit variadic collection if needed */
            if (e->as.closure.has_variadic) {
                uint8_t var_reg = local_reg((int)(declared_arity + 1));
                emit_ABC(ROP_COLLECT_VARARGS, var_reg, (uint8_t)(declared_arity + 1), 0, line);
            }

            /* Compile body - if block, last expression is the return value */
            if (e->as.closure.body->tag == EXPR_BLOCK) {
                Expr *block = e->as.closure.body;
                if (block->as.block.count > 0 && block->as.block.stmts[block->as.block.count - 1]->tag == STMT_EXPR) {
                    uint8_t result_reg = alloc_reg();
                    for (size_t i = 0; i + 1 < block->as.block.count; i++) compile_stmt(block->as.block.stmts[i]);
                    compile_expr(block->as.block.stmts[block->as.block.count - 1]->as.expr, result_reg, line);
                    emit_return(result_reg, line);
                    free_reg(result_reg);
                } else {
                    for (size_t i = 0; i < block->as.block.count; i++) compile_stmt(block->as.block.stmts[i]);
                    uint8_t result_reg = alloc_reg();
                    emit_ABC(ROP_LOADUNIT, result_reg, 0, 0, line);
                    emit_return(result_reg, line);
                    free_reg(result_reg);
                }
            } else {
                uint8_t result_reg = alloc_reg();
                compile_expr(e->as.closure.body, result_reg, line);
                emit_return(result_reg, line);
                free_reg(result_reg);
            }

            RegChunk *fn_chunk = func_comp.chunk;
            size_t upvalue_count = func_comp.upvalue_count;
            RegCompilerUpvalue *upvalues = NULL;
            if (upvalue_count > 0) {
                upvalues = malloc(upvalue_count * sizeof(RegCompilerUpvalue));
                if (!upvalues) return;
                memcpy(upvalues, func_comp.upvalues, upvalue_count * sizeof(RegCompilerUpvalue));
            }
            rc_cleanup(&func_comp);
            rc = func_comp.enclosing;

            /* Store compiled chunk as a closure constant */
            LatValue fn_val;
            memset(&fn_val, 0, sizeof(fn_val));
            fn_val.type = VAL_CLOSURE;
            fn_val.phase = VTAG_UNPHASED;
            fn_val.region_id = REGION_NONE;
            fn_val.as.closure.upvalue_count = (uint32_t)upvalue_count;
            fn_val.as.closure.body = NULL;
            fn_val.as.closure.native_fn = fn_chunk;
            fn_val.as.closure.param_count = e->as.closure.param_count;
            fn_val.as.closure.has_variadic = e->as.closure.has_variadic;
            if (e->as.closure.param_count > 0) {
                fn_val.as.closure.param_names = malloc(e->as.closure.param_count * sizeof(char *));
                if (!fn_val.as.closure.param_names) return;
                for (size_t pi = 0; pi < e->as.closure.param_count; pi++)
                    fn_val.as.closure.param_names[pi] = strdup(e->as.closure.params[pi]);
            }
            uint16_t fn_ki = add_constant(fn_val);

            emit_ABx(ROP_CLOSURE, dst, fn_ki, line);
            /* Emit upvalue descriptors as follow-up instructions */
            for (size_t i = 0; i < upvalue_count; i++) {
                emit_ABC(ROP_MOVE, upvalues[i].is_local ? 1 : 0, upvalues[i].index, 0, line);
            }
            free(upvalues);
            break;
        }

        case EXPR_INTERP_STRING: {
            /* Build interpolated string by concatenating parts */
            if (e->as.interp.count == 0) {
                uint16_t ki = add_constant(value_string(e->as.interp.parts[0]));
                emit_ABx(ROP_LOADK, dst, ki, line);
                break;
            }

            /* Load first part */
            uint16_t ki = add_constant(value_string(e->as.interp.parts[0]));
            emit_ABx(ROP_LOADK, dst, ki, line);

            uint8_t tmp = alloc_reg();
            for (size_t i = 0; i < e->as.interp.count; i++) {
                /* Concat with expression result */
                compile_expr(e->as.interp.exprs[i], tmp, line);
                emit_ABC(ROP_CONCAT, dst, dst, tmp, line);
                /* Concat with next string part */
                if (e->as.interp.parts[i + 1][0] != '\0') {
                    uint16_t pk = add_constant(value_string(e->as.interp.parts[i + 1]));
                    emit_ABx(ROP_LOADK, tmp, pk, line);
                    emit_ABC(ROP_CONCAT, dst, dst, tmp, line);
                }
            }
            free_reg(tmp);
            break;
        }

        case EXPR_MATCH: {
            uint8_t scrutinee = alloc_reg();
            compile_expr(e->as.match_expr.scrutinee, scrutinee, line);

            size_t arm_count = e->as.match_expr.arm_count;
            size_t *end_jumps = arm_count ? malloc(arm_count * sizeof(size_t)) : NULL;
            if (arm_count && !end_jumps) {
                if (!rc_error) rc_error = strdup("out of memory compiling match expression");
                free_reg(scrutinee);
                break;
            }
            size_t end_count = 0;

            for (size_t i = 0; i < arm_count; i++) {
                MatchArm *arm = &e->as.match_expr.arms[i];
                uint8_t matched = alloc_reg();

                /* Pattern bindings and body locals share one arm-only scope.
                 * Both success and failure execute the same cleanup sequence. */
                begin_scope();
                declare_pattern_bindings(arm->pattern);
                compile_pattern_match(arm->pattern, scrutinee, matched, line);
                size_t pattern_fail = emit_jump_placeholder(ROP_JMPFALSE, matched, line);

                size_t guard_fail = SIZE_MAX;
                if (arm->guard) {
                    uint8_t guard = alloc_reg();
                    compile_expr(arm->guard, guard, line);
                    guard_fail = emit_jump_placeholder(ROP_JMPFALSE, guard, line);
                    free_reg(guard);
                }

                if (arm->body_count > 0) {
                    for (size_t j = 0; j + 1 < arm->body_count; j++) compile_stmt(arm->body[j]);
                    Stmt *last = arm->body[arm->body_count - 1];
                    if (last->tag == STMT_EXPR) {
                        compile_expr(last->as.expr, dst, line);
                    } else {
                        compile_stmt(last);
                        emit_ABC(ROP_LOADUNIT, dst, 0, 0, line);
                    }
                } else {
                    emit_ABC(ROP_LOADUNIT, dst, 0, 0, line);
                }

                emit_ABC(ROP_LOADTRUE, matched, 0, 0, line);
                size_t success_to_cleanup = emit_jmp_placeholder(line);

                patch_jump(pattern_fail);
                if (guard_fail != SIZE_MAX) patch_jump(guard_fail);
                emit_ABC(ROP_LOADFALSE, matched, 0, 0, line);
                patch_jmp(success_to_cleanup);

                end_scope(line);
                size_t next_arm = emit_jump_placeholder(ROP_JMPFALSE, matched, line);
                end_jumps[end_count++] = emit_jmp_placeholder(line);
                patch_jump(next_arm);
                free_reg(matched);
            }

            emit_ABC(ROP_LOADNIL, dst, 0, 0, line);
            for (size_t i = 0; i < end_count; i++) patch_jmp(end_jumps[i]);
            free(end_jumps);
            free_reg(scrutinee);
            break;
        }
        case EXPR_ENUM_VARIANT: {
            if (e->as.enum_variant.module_alias) {
                Expr alias_expr;
                memset(&alias_expr, 0, sizeof(alias_expr));
                alias_expr.tag = EXPR_IDENT;
                alias_expr.line = line;
                alias_expr.as.str_val = e->as.enum_variant.module_alias;
                uint8_t metadata_reg = alloc_reg();
                compile_expr(&alias_expr, metadata_reg, line);
                char marker[256];
                snprintf(marker, sizeof(marker), "__enum_%s", e->as.enum_variant.enum_name);
                emit_getfield(metadata_reg, metadata_reg, add_constant(value_string(marker)), line);
                size_t enum_ok = emit_jump_placeholder(ROP_JMPNOTNIL, metadata_reg, line);
                uint8_t err_reg = alloc_reg();
                char err[512];
                snprintf(err, sizeof(err), "module '%s' has no enum '%s'", e->as.enum_variant.module_alias,
                         e->as.enum_variant.enum_name);
                emit_ABx(ROP_LOADK, err_reg, add_constant(value_string(err)), line);
                emit_ABC(ROP_THROW, err_reg, 0, 0, line);
                free_reg(err_reg);
                patch_jump(enum_ok);
                emit_check_enum_variant(metadata_reg, e->as.enum_variant.variant_name, e->as.enum_variant.arg_count,
                                        line);
                compile_enum_construction(e, dst, line);
                free_reg(metadata_reg);
                break;
            }
            if (rc_is_imported_enum_candidate(e->as.enum_variant.enum_name)) {
                Expr metadata_expr;
                memset(&metadata_expr, 0, sizeof(metadata_expr));
                metadata_expr.tag = EXPR_IDENT;
                metadata_expr.line = line;
                metadata_expr.as.str_val = e->as.enum_variant.enum_name;
                uint8_t metadata_reg = alloc_reg();
                compile_expr(&metadata_expr, metadata_reg, line);
                emit_check_enum_variant(metadata_reg, e->as.enum_variant.variant_name, e->as.enum_variant.arg_count,
                                        line);
                compile_enum_construction(e, dst, line);
                free_reg(metadata_reg);
                break;
            }
            if (rc_is_known_enum(e->as.enum_variant.enum_name) &&
                !rc_known_enum_variant_ok(e->as.enum_variant.enum_name, e->as.enum_variant.variant_name,
                                          e->as.enum_variant.arg_count)) {
                rc_error = strdup("unknown enum variant or incorrect payload arity");
                emit_ABC(ROP_LOADNIL, dst, 0, 0, line);
                break;
            }
            if (!rc_is_known_enum(e->as.enum_variant.enum_name)) {
                /* Not a declared enum — fall back to global function call
                 * e.g. Map::new() calls the "Map::new" builtin */
                char key[256];
                snprintf(key, sizeof(key), "%s::%s", e->as.enum_variant.enum_name, e->as.enum_variant.variant_name);
                uint16_t fn_ki = add_constant(value_string(key));
                uint8_t fn_reg = alloc_reg();
                emit_ABx(ROP_GETGLOBAL, fn_reg, fn_ki, line);

                /* Compile args into contiguous registers after fn_reg */
                for (size_t i = 0; i < e->as.enum_variant.arg_count; i++) {
                    uint8_t arg_reg = alloc_reg();
                    compile_expr(e->as.enum_variant.args[i], arg_reg, line);
                }
                emit_ABC(ROP_CALL, fn_reg, (uint8_t)e->as.enum_variant.arg_count, 0, line);
                if (dst != fn_reg) emit_ABC(ROP_MOVE, dst, fn_reg, 0, line);
                free_regs_to(fn_reg);
                break;
            }
            /* For simple enums without args, just load the constant */
            if (e->as.enum_variant.arg_count == 0) {
                uint16_t ki =
                    add_constant(value_enum(e->as.enum_variant.enum_name, e->as.enum_variant.variant_name, NULL, 0));
                emit_ABx(ROP_LOADK, dst, ki, line);
            } else {
                /* Compile payload args into contiguous registers */
                uint8_t base = alloc_reg();
                for (size_t i = 0; i < e->as.enum_variant.arg_count; i++) {
                    uint8_t arg_reg = (i == 0) ? base : alloc_reg();
                    compile_expr(e->as.enum_variant.args[i], arg_reg, line);
                }
                /* NEWENUM: A=dst, B=name_ki, C=argc
                 * Follow-up data word: A=base, B=variant_ki */
                uint16_t name_ki = add_constant(value_string(e->as.enum_variant.enum_name));
                uint16_t var_ki = add_constant(value_string(e->as.enum_variant.variant_name));
                /* enum name is full 16-bit (lo in B, hi in the data word's C);
                 * the variant key occupies the data word's B with no spare bits
                 * (LAT-463: refuse rather than truncate). */
                emit_ABC(ROP_NEWENUM, dst, (uint8_t)(name_ki & 0xFF), (uint8_t)e->as.enum_variant.arg_count, line);
                emit_ABC(ROP_MOVE, base, name_ki8(var_ki), (uint8_t)((name_ki >> 8) & 0xFF), line);
                free_regs_to(base);
            }
            break;
        }

        case EXPR_SUBLIMATE: {
            compile_expr(e->as.freeze_expr, dst, line);
            if (e->as.freeze_expr->tag == EXPR_IDENT) {
                const char *name = e->as.freeze_expr->as.str_val;
                uint16_t name_ki = add_constant(value_string(name));
                int slot = resolve_local(rc, name);
                if (slot >= 0) {
                    emit_ABC(ROP_SUBLIMATE_VAR, name_ki8(name_ki), 0, local_reg(slot), line);
                } else {
                    int uv = resolve_upvalue(rc, name);
                    if (uv >= 0) {
                        emit_ABC(ROP_SUBLIMATE_VAR, name_ki8(name_ki), 1, (uint8_t)uv, line);
                    } else {
                        emit_ABC(ROP_SUBLIMATE_VAR, name_ki8(name_ki), 2, 0, line);
                    }
                }
            }
            /* For non-ident expressions, set the phase on the register value */
            emit_ABC(ROP_SUBLIMATE, dst, 0, 0, line);
            break;
        }

        case EXPR_TUPLE: {
            /* Compile elements into contiguous registers, then NEWTUPLE */
            if (e->as.tuple.count == 0) {
                emit_ABC(ROP_NEWTUPLE, dst, 0, 0, line);
            } else {
                uint8_t base = alloc_reg();
                for (size_t i = 0; i < e->as.tuple.count; i++) {
                    uint8_t elem_reg = (i == 0) ? base : alloc_reg();
                    compile_expr(e->as.tuple.elems[i], elem_reg, line);
                }
                emit_ABC(ROP_NEWTUPLE, dst, base, (uint8_t)e->as.tuple.count, line);
                free_regs_to(base);
            }
            break;
        }

        case EXPR_SPREAD: {
            /* Compile inner expr, then flatten */
            compile_expr(e->as.spread_expr, dst, line);
            emit_ABC(ROP_ARRAY_FLATTEN, dst, dst, 0, line);
            break;
        }

        case EXPR_TRY_CATCH: {
            /* PUSH_HANDLER A=error_reg, sBx=offset to catch
             * try body
             * POP_HANDLER
             * JMP past catch
             * catch body */
            uint8_t error_reg = alloc_reg();
            size_t handler = emit_jump_placeholder(ROP_PUSH_HANDLER, error_reg, line);
            rc->handler_depth++;

            /* Try body */
            begin_scope();
            if (e->as.try_catch.try_count > 0) {
                for (size_t i = 0; i + 1 < e->as.try_catch.try_count; i++) compile_stmt(e->as.try_catch.try_stmts[i]);
                Stmt *last = e->as.try_catch.try_stmts[e->as.try_catch.try_count - 1];
                if (last->tag == STMT_EXPR) compile_expr(last->as.expr, dst, line);
                else {
                    compile_stmt(last);
                    emit_ABC(ROP_LOADUNIT, dst, 0, 0, line);
                }
            } else {
                emit_ABC(ROP_LOADUNIT, dst, 0, 0, line);
            }
            end_scope(line);

            emit_ABC(ROP_POP_HANDLER, 0, 0, 0, line);
            rc->handler_depth--;
            size_t skip_catch = emit_jmp_placeholder(line);
            patch_jump(handler);

            /* Catch body — bind error to catch_var */
            begin_scope();
            if (e->as.try_catch.catch_var) {
                uint8_t catch_reg = add_local(e->as.try_catch.catch_var);
                emit_ABC(ROP_MOVE, catch_reg, error_reg, 0, line);
            }
            if (e->as.try_catch.catch_count > 0) {
                for (size_t i = 0; i + 1 < e->as.try_catch.catch_count; i++)
                    compile_stmt(e->as.try_catch.catch_stmts[i]);
                Stmt *last = e->as.try_catch.catch_stmts[e->as.try_catch.catch_count - 1];
                if (last->tag == STMT_EXPR) compile_expr(last->as.expr, dst, line);
                else {
                    compile_stmt(last);
                    emit_ABC(ROP_LOADUNIT, dst, 0, 0, line);
                }
            } else {
                emit_ABC(ROP_LOADUNIT, dst, 0, 0, line);
            }
            end_scope(line);
            free_reg(error_reg);
            patch_jmp(skip_catch);
            break;
        }

        case EXPR_TRY_PROPAGATE: {
            /* Compile inner expr, then TRY_UNWRAP */
            compile_expr(e->as.try_propagate_expr, dst, line);
            emit_ABC(ROP_TRY_UNWRAP, dst, 0, 0, line);
            break;
        }

        case EXPR_FORGE: {
            /* Forge: compile block, then freeze result */
            begin_scope();
            if (e->as.block.count > 0) {
                for (size_t i = 0; i + 1 < e->as.block.count; i++) compile_stmt(e->as.block.stmts[i]);
                Stmt *last = e->as.block.stmts[e->as.block.count - 1];
                if (last->tag == STMT_EXPR) compile_expr(last->as.expr, dst, line);
                else {
                    compile_stmt(last);
                    emit_ABC(ROP_LOADUNIT, dst, 0, 0, line);
                }
            } else {
                emit_ABC(ROP_LOADUNIT, dst, 0, 0, line);
            }
            end_scope(line);
            emit_ABC(ROP_FREEZE, dst, dst, 0, line);
            break;
        }

        case EXPR_ANNEAL: {
            /* Anneal: check crystal, thaw, apply closure, refreeze, write-back */

            /* 1. Check: target must be crystal */
            compile_expr(e->as.anneal.expr, dst, line);
            uint8_t check_reg = alloc_reg();
            emit_ABC(ROP_IS_CRYSTAL, check_reg, dst, 0, line);
            size_t ok_jump = emit_jump_placeholder(ROP_JMPTRUE, check_reg, line);
            free_reg(check_reg);
            /* Not crystal → throw error */
            {
                uint8_t err_reg2 = alloc_reg();
                uint16_t err_ki = add_constant(value_string("anneal requires a crystal value"));
                emit_ABx(ROP_LOADK, err_reg2, err_ki, line);
                emit_ABC(ROP_THROW, err_reg2, 0, 0, line);
                free_reg(err_reg2);
            }
            patch_jump(ok_jump);

            /* 2. Wrap in try/catch for error prefix */
            uint8_t err_reg = alloc_reg();
            size_t handler = emit_jump_placeholder(ROP_PUSH_HANDLER, err_reg, line);
            rc->handler_depth++;

            /* 3. Call closure with thawed value */
            uint8_t fn_reg = alloc_reg();
            uint8_t arg_reg = alloc_reg(); /* must be fn_reg+1 for CALL convention */
            compile_expr(e->as.anneal.closure, fn_reg, line);
            emit_ABC(ROP_THAW, arg_reg, dst, 0, line);
            emit_ABC(ROP_CALL, fn_reg, 1, 1, line);
            /* Result is in fn_reg; move to dst */
            emit_ABC(ROP_MOVE, dst, fn_reg, 0, line);
            free_reg(arg_reg);
            free_reg(fn_reg);

            /* 4. Write-back result to variable register, then freeze */
            if (e->as.anneal.expr->tag == EXPR_IDENT) {
                const char *name = e->as.anneal.expr->as.str_val;
                uint16_t name_ki = add_constant(value_string(name));
                int slot = resolve_local(rc, name);
                if (slot >= 0) {
                    /* Write closure result back to the variable's register */
                    emit_ABC(ROP_MOVE, local_reg(slot), dst, 0, line);
                    emit_ABC(ROP_FREEZE_VAR, name_ki8(name_ki), 0, local_reg(slot), line);
                } else {
                    int uv = resolve_upvalue(rc, name);
                    if (uv >= 0) {
                        emit_ABC(ROP_FREEZE_VAR, name_ki8(name_ki), 1, (uint8_t)uv, line);
                    } else {
                        emit_ABC(ROP_FREEZE_VAR, name_ki8(name_ki), 2, 0, line);
                    }
                }
            } else {
                emit_ABC(ROP_FREEZE, dst, dst, 0, line);
            }

            /* 5. End try — pop handler, jump past catch */
            emit_ABC(ROP_POP_HANDLER, 0, 0, 0, line);
            rc->handler_depth--;
            size_t past_catch = emit_jmp_placeholder(line);

            /* 6. Catch: wrap with "anneal failed: " and rethrow */
            patch_jump(handler);
            {
                uint8_t prefix_reg = alloc_reg();
                uint16_t prefix_ki = add_constant(value_string("anneal failed: "));
                emit_ABx(ROP_LOADK, prefix_reg, prefix_ki, line);
                emit_ABC(ROP_CONCAT, err_reg, prefix_reg, err_reg, line);
                free_reg(prefix_reg);
                emit_ABC(ROP_THROW, err_reg, 0, 0, line);
            }
            free_reg(err_reg);
            patch_jmp(past_catch);
            break;
        }

        case EXPR_CRYSTALLIZE: {
            /* crystallize(x) { body } — freeze x, run body, thaw x back (if wasn't crystal) */
            if (e->as.crystallize.expr->tag == EXPR_IDENT) {
                const char *name = e->as.crystallize.expr->as.str_val;
                uint16_t name_ki = add_constant(value_string(name));
                int slot = resolve_local(rc, name);
                uint8_t loc_type, slot_val;
                if (slot >= 0) {
                    loc_type = 0;
                    slot_val = local_reg(slot);
                } else {
                    int uv = resolve_upvalue(rc, name);
                    if (uv >= 0) {
                        loc_type = 1;
                        slot_val = (uint8_t)uv;
                    } else {
                        loc_type = 2;
                        slot_val = 0;
                    }
                }
                /* Check if already crystal before freezing */
                compile_expr(e->as.crystallize.expr, dst, line);
                uint8_t was_crystal = alloc_reg();
                emit_ABC(ROP_IS_CRYSTAL, was_crystal, dst, 0, line);
                /* Freeze the variable */
                compile_expr(e->as.crystallize.expr, dst, line);
                emit_ABC(ROP_FREEZE_VAR, name_ki8(name_ki), loc_type, slot_val, line);
                /* Execute body */
                begin_scope();
                for (size_t i = 0; i < e->as.crystallize.body_count; i++) compile_stmt(e->as.crystallize.body[i]);
                end_scope(line);
                /* Conditional thaw: only if was NOT already crystal */
                size_t skip_thaw = emit_jump_placeholder(ROP_JMPTRUE, was_crystal, line);
                /* was not crystal → thaw back */
                compile_expr(e->as.crystallize.expr, dst, line);
                emit_ABC(ROP_THAW_VAR, name_ki8(name_ki), loc_type, slot_val, line);
                patch_jump(skip_thaw);
                free_reg(was_crystal);
            } else {
                /* Non-ident: just freeze value, run body, refreeze */
                compile_expr(e->as.crystallize.expr, dst, line);
                emit_ABC(ROP_FREEZE, dst, dst, 0, line);
                begin_scope();
                for (size_t i = 0; i < e->as.crystallize.body_count; i++) compile_stmt(e->as.crystallize.body[i]);
                end_scope(line);
            }
            break;
        }

        case EXPR_BORROW: {
            /* borrow(x) { body } — thaw x, run body, freeze x back (if wasn't fluid) */
            if (e->as.borrow.expr->tag == EXPR_IDENT) {
                const char *name = e->as.borrow.expr->as.str_val;
                uint16_t name_ki = add_constant(value_string(name));
                int slot = resolve_local(rc, name);
                uint8_t loc_type, slot_val;
                if (slot >= 0) {
                    loc_type = 0;
                    slot_val = local_reg(slot);
                } else {
                    int uv = resolve_upvalue(rc, name);
                    if (uv >= 0) {
                        loc_type = 1;
                        slot_val = (uint8_t)uv;
                    } else {
                        loc_type = 2;
                        slot_val = 0;
                    }
                }
                /* Check if already fluid before thawing */
                compile_expr(e->as.borrow.expr, dst, line);
                uint8_t was_fluid = alloc_reg();
                emit_ABC(ROP_IS_FLUID, was_fluid, dst, 0, line);
                /* Thaw the variable */
                compile_expr(e->as.borrow.expr, dst, line);
                emit_ABC(ROP_THAW_VAR, name_ki8(name_ki), loc_type, slot_val, line);
                /* Execute body */
                begin_scope();
                for (size_t i = 0; i < e->as.borrow.body_count; i++) compile_stmt(e->as.borrow.body[i]);
                end_scope(line);
                /* Conditional freeze: only if was NOT already fluid */
                size_t skip_freeze = emit_jump_placeholder(ROP_JMPTRUE, was_fluid, line);
                /* was not fluid → freeze back */
                compile_expr(e->as.borrow.expr, dst, line);
                emit_ABC(ROP_FREEZE_VAR, name_ki8(name_ki), loc_type, slot_val, line);
                patch_jump(skip_freeze);
                free_reg(was_fluid);
            } else {
                /* Non-ident: just thaw value, run body */
                compile_expr(e->as.borrow.expr, dst, line);
                emit_ABC(ROP_THAW, dst, dst, 0, line);
                begin_scope();
                for (size_t i = 0; i < e->as.borrow.body_count; i++) compile_stmt(e->as.borrow.body[i]);
                end_scope(line);
            }
            break;
        }

        case EXPR_SPAWN: {
            /* Spawn outside scope — compile as ROP_SCOPE with 0 spawns (sub-chunk so return works) */
            RegChunk *spawn_ch = compile_reg_sub_body(e->as.block.stmts, e->as.block.count, line);
            uint8_t body_idx = sub_idx8(add_regchunk_constant(spawn_ch));

            /* Emit: ROP_SCOPE dst, then data word with spawn_count=0, sync_idx=body_idx */
            emit_ABC(ROP_SCOPE, dst, 0, 0, line);
            emit_ABC(0, 0, body_idx, 0, line); /* spawn_count=0, sync_idx=body_idx */
            break;
        }

        case EXPR_SCOPE: {
            size_t total = e->as.block.count;
            size_t spawn_count = 0;
            for (size_t i = 0; i < total; i++) {
                Stmt *stmt = e->as.block.stmts[i];
                if (stmt->tag == STMT_EXPR && stmt->as.expr->tag == EXPR_SPAWN) spawn_count++;
            }

            /* A synchronous scope is an ordinary lexical block. Keeping it
             * in the parent chunk preserves outer-local writes and lets
             * return/break/continue use the enclosing control-flow targets. */
            if (spawn_count == 0) {
                begin_scope();
                if (total > 0) {
                    for (size_t i = 0; i + 1 < total; i++) compile_stmt(e->as.block.stmts[i]);
                    Stmt *last = e->as.block.stmts[total - 1];
                    if (last->tag == STMT_EXPR) compile_expr(last->as.expr, dst, line);
                    else {
                        compile_stmt(last);
                        emit_ABC(ROP_LOADUNIT, dst, 0, 0, line);
                    }
                } else {
                    emit_ABC(ROP_LOADUNIT, dst, 0, 0, line);
                }
                end_scope(line);
                break;
            }

#ifdef __EMSCRIPTEN__
            /* WASM fallback: run as synchronous block */
            for (size_t i = 0; i < e->as.block.count; i++) compile_stmt(e->as.block.stmts[i]);
            emit_ABC(ROP_LOADUNIT, dst, 0, 0, line);
#else
            /* Synchronous statements belong to the enclosing control-flow
             * frame even when the scope also contains spawns. Compile them
             * inline so writes, return/break/continue, handlers, and lexical
             * defer cleanup retain their ordinary block semantics. ROP_SCOPE
             * below is then only responsible for running and joining children. */
            begin_scope();
            uint8_t sync_idx = 0xFF;
            for (size_t i = 0; i < total; i++) {
                Stmt *s = e->as.block.stmts[i];
                if (!(s->tag == STMT_EXPR && s->as.expr->tag == EXPR_SPAWN)) compile_stmt(s);
            }

            /* Compile each spawn body */
            uint8_t *spawn_indices = NULL;
            if (spawn_count > 0) {
                spawn_indices = malloc(spawn_count * sizeof(uint8_t));
                if (!spawn_indices) return;
            }
            size_t spi = 0;
            for (size_t i = 0; i < total; i++) {
                Stmt *s = e->as.block.stmts[i];
                if (s->tag == STMT_EXPR && s->as.expr->tag == EXPR_SPAWN) {
                    Expr *spawn_expr = s->as.expr;
                    RegChunk *spawn_ch =
                        compile_reg_sub_body(spawn_expr->as.block.stmts, spawn_expr->as.block.count, line);
                    spawn_indices[spi++] = sub_idx8(add_regchunk_constant(spawn_ch));
                }
            }

            /* Emit: ROP_SCOPE dst, then data words with spawn_count, sync_idx, spawn_indices */
            emit_ABC(ROP_SCOPE, dst, 0, 0, line);
            /* Data word 1: spawn_count in A, sync_idx in B */
            emit_ABC(0, (uint8_t)spawn_count, sync_idx, 0, line);
            /* Spawn indices packed 3 per data word */
            for (size_t i = 0; i < spawn_count; i += 3) {
                uint8_t a = spawn_indices[i];
                uint8_t b = (i + 1 < spawn_count) ? spawn_indices[i + 1] : 0;
                uint8_t c = (i + 2 < spawn_count) ? spawn_indices[i + 2] : 0;
                emit_ABC(0, a, b, c, line);
            }
            free(spawn_indices);
            end_scope(line);
#endif
            break;
        }

        case EXPR_SELECT: {
            size_t arm_count = e->as.select_expr.arm_count;
            SelectArm *arms = e->as.select_expr.arms;
            if (arm_count > UINT8_MAX) {
                if (!rc_error) rc_error = strdup("select has too many arms (max 255)");
                emit_ABC(ROP_LOADUNIT, dst, 0, 0, line);
                break;
            }

            uint8_t operand_base = 0;
            uint8_t flags[UINT8_MAX] = {0};
            for (size_t i = 0; i < arm_count; i++) {
                uint8_t operand = alloc_reg();
                if (i == 0) operand_base = operand;
                if (arms[i].is_default) {
                    flags[i] |= 0x01;
                    emit_ABC(ROP_LOADUNIT, operand, 0, 0, line);
                } else if (arms[i].is_timeout) {
                    flags[i] |= 0x02;
                    compile_expr(arms[i].timeout_expr, operand, line);
                } else {
                    compile_expr(arms[i].channel_expr, operand, line);
                }
                if (arms[i].binding_name) flags[i] |= 0x04;
            }

            emit_ABC(ROP_SELECT_CHOOSE, dst, operand_base, (uint8_t)arm_count, line);
            for (size_t i = 0; i < arm_count; i += 3) {
                uint8_t a = flags[i];
                uint8_t b = i + 1 < arm_count ? flags[i + 1] : 0;
                uint8_t c = i + 2 < arm_count ? flags[i + 2] : 0;
                emit_ABC(0, a, b, c, line);
            }
            if (arm_count > 0) free_regs_to(operand_base);

            uint8_t pair_index = alloc_reg();
            uint8_t zero = alloc_reg();
            emit_AsBx(ROP_LOADI, zero, 0, line);
            emit_ABC(ROP_GETINDEX, pair_index, dst, zero, line);
            free_reg(zero);

            size_t *end_jumps = arm_count > 0 ? malloc(arm_count * sizeof(size_t)) : NULL;
            if (arm_count > 0 && !end_jumps) {
                rc_error = strdup("out of memory compiling select");
                free_reg(pair_index);
                break;
            }

            size_t end_count = 0;
            for (size_t i = 0; i < arm_count; i++) {
                uint8_t expected = alloc_reg();
                uint8_t selected = alloc_reg();
                emit_AsBx(ROP_LOADI, expected, (int16_t)i, line);
                emit_ABC(ROP_EQ, selected, pair_index, expected, line);
                size_t next_arm = emit_jump_placeholder(ROP_JMPFALSE, selected, line);
                free_reg(selected);
                free_reg(expected);

                begin_scope();
                if (arms[i].binding_name) {
                    uint8_t binding = add_local(arms[i].binding_name);
                    uint8_t value_index = alloc_reg();
                    emit_AsBx(ROP_LOADI, value_index, 1, line);
                    emit_ABC(ROP_GETINDEX, binding, dst, value_index, line);
                    free_reg(value_index);
                }

                if (arms[i].body_count > 0) {
                    for (size_t j = 0; j + 1 < arms[i].body_count; j++) compile_stmt(arms[i].body[j]);
                    Stmt *last = arms[i].body[arms[i].body_count - 1];
                    if (last->tag == STMT_EXPR) compile_expr(last->as.expr, dst, line);
                    else {
                        compile_stmt(last);
                        emit_ABC(ROP_LOADUNIT, dst, 0, 0, line);
                    }
                } else {
                    emit_ABC(ROP_LOADUNIT, dst, 0, 0, line);
                }
                end_scope(line);
                end_jumps[end_count++] = emit_jmp_placeholder(line);
                patch_jump(next_arm);
            }

            emit_ABC(ROP_LOADUNIT, dst, 0, 0, line);
            for (size_t i = 0; i < end_count; i++) patch_jmp(end_jumps[i]);
            free(end_jumps);
            free_reg(pair_index);
            break;
        }

        default:
            /* Unsupported expression — record a compile error (first one wins) */
            if (!rc_error) {
                char buf[128];
                snprintf(buf, sizeof(buf), "unsupported expression type %d in regvm compiler", e->tag);
                rc_error = strdup(buf);
            }
            break;
    }
}

/* Write back a modified value through a chain of nested EXPR_INDEX nodes.
 * `node` is the EXPR_INDEX that produced `modified_reg` (via GETINDEX).
 * This emits SETINDEX to propagate the change back to the root variable. */
static void emit_index_writeback(Expr *node, uint8_t modified_reg, int line) {
    if (node->tag != EXPR_INDEX) return;
    Expr *parent = node->as.index.object;

    if (parent->tag == EXPR_IDENT) {
        /* Base case: parent is a variable — write modified_reg back into parent[index] */
        const char *name = parent->as.str_val;
        int local = resolve_local(rc, name);
        uint8_t idx_reg = alloc_reg();
        compile_expr(node->as.index.index, idx_reg, line);
        if (local >= 0) {
            emit_ABC(ROP_SETINDEX_LOCAL, local_reg(local), idx_reg, modified_reg, line);
        } else {
            uint8_t parent_reg = alloc_reg();
            compile_expr(parent, parent_reg, line);
            emit_ABC(ROP_SETINDEX, parent_reg, idx_reg, modified_reg, line);
            int uv = resolve_upvalue(rc, name);
            if (uv >= 0) {
                emit_ABC(ROP_SETUPVALUE, parent_reg, (uint8_t)uv, 0, line);
            } else {
                uint16_t nki = add_constant(value_string(name));
                emit_ABx(ROP_SETGLOBAL, parent_reg, nki, line);
            }
            free_reg(parent_reg);
        }
        free_reg(idx_reg);
    } else if (parent->tag == EXPR_INDEX) {
        /* Recursive case: parent is itself an index — set, then recurse */
        uint8_t parent_reg = alloc_reg();
        compile_expr(parent, parent_reg, line);
        uint8_t idx_reg = alloc_reg();
        compile_expr(node->as.index.index, idx_reg, line);
        emit_ABC(ROP_SETINDEX, parent_reg, idx_reg, modified_reg, line);
        free_reg(idx_reg);
        emit_index_writeback(parent, parent_reg, line);
        free_reg(parent_reg);
    }
}

/* ── Statement compilation ── */

static void compile_stmt(const Stmt *s) {
    if (rc_error) return;
    int line = s->line;

    switch (s->tag) {
        case STMT_EXPR: {
            /* Compile expression, result goes into a temporary that we free */
            uint8_t tmp = alloc_reg();
            g_reg_stmt_tos = true; /* LAT-545: statement-top expression */
            compile_expr(s->as.expr, tmp, line);
            free_reg(tmp);
            break;
        }

        case STMT_BINDING: {
            if (rc->scope_depth > 0) {
                /* Local variable: allocate a register */
                uint8_t reg = add_local(s->as.binding.name);
                if (s->as.binding.value) {
                    g_reg_stmt_tos = true; /* LAT-545 */
                    compile_expr(s->as.binding.value, reg, line);
                } else emit_ABC(ROP_LOADNIL, reg, 0, 0, line);

                if (s->as.binding.phase == PHASE_FLUID) emit_ABC(ROP_MARKFLUID, reg, 0, 0, line);
                else if (s->as.binding.phase == PHASE_CRYSTAL) emit_ABC(ROP_FREEZE, reg, reg, 0, line);
            } else {
                /* Global variable */
                uint8_t tmp = alloc_reg();
                if (s->as.binding.value) {
                    g_reg_stmt_tos = true; /* LAT-545 */
                    compile_expr(s->as.binding.value, tmp, line);
                } else emit_ABC(ROP_LOADNIL, tmp, 0, 0, line);

                if (s->as.binding.phase == PHASE_FLUID) emit_ABC(ROP_MARKFLUID, tmp, 0, 0, line);
                else if (s->as.binding.phase == PHASE_CRYSTAL) emit_ABC(ROP_FREEZE, tmp, tmp, 0, line);

                uint16_t name_ki = add_constant(value_string(s->as.binding.name));
                emit_ABx(ROP_DEFINEGLOBAL, tmp, name_ki, line);
                free_reg(tmp);
            }
            break;
        }

        case STMT_ASSIGN: {
            g_reg_stmt_tos = true; /* LAT-545: the assigned value is a statement-top expr */
            if (s->as.assign.target->tag == EXPR_IDENT) {
                const char *name = s->as.assign.target->as.str_val;
                int local = resolve_local(rc, name);
                if (local >= 0) {
                    /* LAT-454: an in-place compound assignment into a local
                     * (x = x OP rhs) mutates a crystal/sublimated value in
                     * place. The stack VM rejects these via its
                     * OP_INC_LOCAL/OP_DEC_LOCAL/OP_APPEND_STR_LOCAL fast
                     * opcodes; guard the identical set here so both VMs agree.
                     * Set: `x = x + <any>` (covers += and ++), and `x = x - 1`
                     * (covers --). Other rewrites (x = x - 2, x = x * y) stay
                     * fresh-value rebinds on both backends. */
                    Expr *v = s->as.assign.value;
                    if (v->tag == EXPR_BINOP && v->as.binop.left->tag == EXPR_IDENT &&
                        strcmp(v->as.binop.left->as.str_val, name) == 0 &&
                        (v->as.binop.op == BINOP_ADD ||
                         (v->as.binop.op == BINOP_SUB && v->as.binop.right->tag == EXPR_INT_LIT &&
                          v->as.binop.right->as.int_val == 1))) {
                        emit_ABC(ROP_CHECK_MUTABLE, local_reg(local), 0, 0, line);
                    }
                    compile_expr(s->as.assign.value, local_reg(local), line);
                } else {
                    int upvalue = resolve_upvalue(rc, name);
                    if (upvalue >= 0) {
                        uint8_t tmp = alloc_reg();
                        compile_expr(s->as.assign.value, tmp, line);
                        emit_ABC(ROP_SETUPVALUE, tmp, (uint8_t)upvalue, 0, line);
                        free_reg(tmp);
                    } else {
                        uint8_t tmp = alloc_reg();
                        compile_expr(s->as.assign.value, tmp, line);
                        uint16_t name_ki = add_constant(value_string(name));
                        emit_ABx(ROP_SETGLOBAL, tmp, name_ki, line);
                        free_reg(tmp);
                    }
                }
            } else if (s->as.assign.target->tag == EXPR_FIELD_ACCESS) {
                Expr *target = s->as.assign.target;
                uint8_t val_reg = alloc_reg();
                compile_expr(s->as.assign.value, val_reg, line);

                /* LAT-545: `arr[f()].x = v` — the object chain's impure indices are
                 * evaluated by both the read (into obj_reg) and the write-back;
                 * hoist them so each fires once and read/write-back agree. */
                bool fa_hoist = target->as.field_access.object->tag == EXPR_INDEX &&
                                index_chain_writable(target->as.field_access.object) &&
                                chain_has_impure_index(target->as.field_access.object);
                RegIdxHoist fa_h;
                if (fa_hoist) reg_idx_hoist_begin(&fa_h, target->as.field_access.object, line);

                /* Get the object register */
                uint8_t obj_reg;
                bool obj_is_local = false;
                int local = -1;
                if (target->as.field_access.object->tag == EXPR_IDENT) {
                    local = resolve_local(rc, target->as.field_access.object->as.str_val);
                    if (local >= 0) {
                        obj_reg = local_reg(local);
                        obj_is_local = true;
                    } else {
                        obj_reg = alloc_reg();
                        compile_expr(target->as.field_access.object, obj_reg, line);
                    }
                } else {
                    obj_reg = alloc_reg();
                    compile_expr(target->as.field_access.object, obj_reg, line);
                }

                uint16_t field_ki = add_constant(value_string(target->as.field_access.field));
                emit_setfield(obj_reg, field_ki, val_reg, line);

                /* Write back if it's a global/upvalue */
                if (!obj_is_local && target->as.field_access.object->tag == EXPR_IDENT) {
                    const char *name = target->as.field_access.object->as.str_val;
                    int uv = resolve_upvalue(rc, name);
                    if (uv >= 0) {
                        emit_ABC(ROP_SETUPVALUE, obj_reg, (uint8_t)uv, 0, line);
                    } else {
                        uint16_t nki = add_constant(value_string(name));
                        emit_ABx(ROP_SETGLOBAL, obj_reg, nki, line);
                    }
                    free_reg(obj_reg);
                } else if (!obj_is_local && target->as.field_access.object->tag == EXPR_INDEX &&
                           index_chain_writable(target->as.field_access.object)) {
                    /* LAT-544: obj[i].x = v. SETFIELD mutated the GETINDEX clone
                     * in obj_reg; propagate it back into obj[i]. A field-assign is
                     * unconditionally a real mutation, so the write-back is
                     * emitted directly (no skip mechanism). A frozen parent/clone
                     * errors in SETFIELD's own phase guard before this runs. */
                    emit_index_writeback(target->as.field_access.object, obj_reg, line);
                    free_reg(obj_reg);
                }
                if (fa_hoist) reg_idx_hoist_end(&fa_h); /* reclaim cached-index regs */
                free_reg(val_reg);
            } else if (s->as.assign.target->tag == EXPR_INDEX &&
                       s->as.assign.target->as.index.index->tag == EXPR_RANGE) {
                /* Slice assignment: arr[start..end] = rhs_array */
                Expr *target = s->as.assign.target;
                Expr *range_expr = target->as.index.index;

                /* LAT-444: reject a slice assignment through a nested index chain
                 * (g[0][1..2] = v) — the intermediate clone has no write-back, so
                 * it would silently no-op. Single-level slice (g[1..2]) is fine. */
                if (target->as.index.object->tag == EXPR_INDEX) {
                    if (!rc_error) rc_error = strdup("slice assignment through a nested index chain is not supported");
                    break;
                }

                uint8_t val_reg = alloc_reg();
                compile_expr(s->as.assign.value, val_reg, line);

                uint8_t obj_reg;
                bool obj_is_local = false;
                if (target->as.index.object->tag == EXPR_IDENT) {
                    int local = resolve_local(rc, target->as.index.object->as.str_val);
                    if (local >= 0) {
                        obj_reg = local_reg(local);
                        obj_is_local = true;
                    } else {
                        obj_reg = alloc_reg();
                        compile_expr(target->as.index.object, obj_reg, line);
                    }
                } else {
                    obj_reg = alloc_reg();
                    compile_expr(target->as.index.object, obj_reg, line);
                }

                /* Allocate consecutive registers for start and end */
                uint8_t start_reg = alloc_reg();
                uint8_t end_reg = alloc_reg();
                compile_expr(range_expr->as.range.start, start_reg, line);
                compile_expr(range_expr->as.range.end, end_reg, line);

                emit_ABC(obj_is_local ? ROP_SETSLICE_LOCAL : ROP_SETSLICE, obj_reg, start_reg, val_reg, line);

                if (!obj_is_local && target->as.index.object->tag == EXPR_IDENT) {
                    const char *name = target->as.index.object->as.str_val;
                    int uv = resolve_upvalue(rc, name);
                    if (uv >= 0) {
                        emit_ABC(ROP_SETUPVALUE, obj_reg, (uint8_t)uv, 0, line);
                    } else {
                        uint16_t nki = add_constant(value_string(name));
                        emit_ABx(ROP_SETGLOBAL, obj_reg, nki, line);
                    }
                    free_reg(obj_reg);
                }
                free_reg(end_reg);
                free_reg(start_reg);
                free_reg(val_reg);
            } else if (s->as.assign.target->tag == EXPR_INDEX) {
                Expr *target = s->as.assign.target;
                uint8_t val_reg = alloc_reg();
                compile_expr(s->as.assign.value, val_reg, line);

                /* LAT-545: `m[f()][j] = v` — the parent chain's impure indices are
                 * evaluated by both the read (into obj_reg) and the write-back;
                 * hoist them so each fires once and read/write-back agree. */
                bool ia_hoist =
                    target->as.index.object->tag == EXPR_INDEX && chain_has_impure_index(target->as.index.object);
                RegIdxHoist ia_h;
                if (ia_hoist) reg_idx_hoist_begin(&ia_h, target->as.index.object, line);

                uint8_t obj_reg;
                bool obj_is_local = false;
                if (target->as.index.object->tag == EXPR_IDENT) {
                    int local = resolve_local(rc, target->as.index.object->as.str_val);
                    if (local >= 0) {
                        obj_reg = local_reg(local);
                        obj_is_local = true;
                    } else {
                        obj_reg = alloc_reg();
                        compile_expr(target->as.index.object, obj_reg, line);
                    }
                } else {
                    obj_reg = alloc_reg();
                    compile_expr(target->as.index.object, obj_reg, line);
                }

                uint8_t idx_reg = alloc_reg();
                compile_expr(target->as.index.index, idx_reg, line);
                emit_ABC(obj_is_local ? ROP_SETINDEX_LOCAL : ROP_SETINDEX, obj_reg, idx_reg, val_reg, line);

                if (!obj_is_local && target->as.index.object->tag == EXPR_IDENT) {
                    const char *name = target->as.index.object->as.str_val;
                    int uv = resolve_upvalue(rc, name);
                    if (uv >= 0) {
                        emit_ABC(ROP_SETUPVALUE, obj_reg, (uint8_t)uv, 0, line);
                    } else {
                        uint16_t nki = add_constant(value_string(name));
                        emit_ABx(ROP_SETGLOBAL, obj_reg, nki, line);
                    }
                    free_reg(obj_reg);
                } else if (!obj_is_local && target->as.index.object->tag == EXPR_INDEX) {
                    /* Nested indexing (e.g. arr[i][j] = val): write modified obj back */
                    emit_index_writeback(target->as.index.object, obj_reg, line);
                    free_reg(obj_reg);
                }
                free_reg(idx_reg);
                if (ia_hoist) reg_idx_hoist_end(&ia_h); /* reclaim cached-index regs */
                free_reg(val_reg);
            }
            break;
        }

        case STMT_RETURN: {
            uint8_t ret_reg = alloc_reg();
            if (s->as.return_expr) {
                g_reg_stmt_tos = true; /* LAT-545 */
                compile_expr(s->as.return_expr, ret_reg, line);
            } else emit_ABC(ROP_LOADUNIT, ret_reg, 0, 0, line);
            emit_postconditions(ret_reg, line);
            emit_return(ret_reg, line);
            free_reg(ret_reg);
            break;
        }

        case STMT_WHILE: {
            size_t saved_break_count = rc->break_count;
            size_t saved_loop_start = rc->loop_start;
            int saved_loop_is_for = rc->loop_is_for;
            int saved_loop_depth = rc->loop_depth;
            uint8_t saved_break_reg = rc->loop_break_reg;
            uint8_t saved_continue_reg = rc->loop_continue_reg;
            size_t saved_break_lc = rc->loop_break_local_count;
            size_t saved_continue_lc = rc->loop_continue_local_count;
            int saved_break_scope_depth = rc->loop_break_scope_depth;
            int saved_continue_scope_depth = rc->loop_continue_scope_depth;
            int saved_break_handler_depth = rc->loop_break_handler_depth;
            int saved_continue_handler_depth = rc->loop_continue_handler_depth;

            rc->loop_break_local_count = rc->local_count;
            rc->loop_continue_local_count = rc->local_count;
            rc->loop_break_reg = rc->next_reg;
            rc->loop_continue_reg = rc->next_reg;
            rc->loop_break_scope_depth = rc->scope_depth;
            rc->loop_continue_scope_depth = rc->scope_depth;
            rc->loop_break_handler_depth = rc->handler_depth;
            rc->loop_continue_handler_depth = rc->handler_depth;
            rc->loop_start = current_chunk()->code_len;
            rc->loop_is_for = 0;
            rc->loop_depth++;

            uint8_t cond_reg = alloc_reg();
            compile_expr(s->as.while_loop.cond, cond_reg, line);
            size_t exit_jump = emit_jump_placeholder(ROP_JMPFALSE, cond_reg, line);
            free_reg(cond_reg);

            begin_scope();
            for (size_t i = 0; i < s->as.while_loop.body_count; i++) compile_stmt(s->as.while_loop.body[i]);
            end_scope(line);

            emit_loop_back(rc->loop_start, line);
            patch_jump(exit_jump);

            for (size_t i = saved_break_count; i < rc->break_count; i++) patch_jmp(rc->break_patches[i]);
            rc->break_count = saved_break_count;
            rc->loop_start = saved_loop_start;
            rc->loop_is_for = saved_loop_is_for;
            rc->loop_depth = saved_loop_depth;
            rc->loop_break_reg = saved_break_reg;
            rc->loop_continue_reg = saved_continue_reg;
            rc->loop_break_local_count = saved_break_lc;
            rc->loop_continue_local_count = saved_continue_lc;
            rc->loop_break_scope_depth = saved_break_scope_depth;
            rc->loop_continue_scope_depth = saved_continue_scope_depth;
            rc->loop_break_handler_depth = saved_break_handler_depth;
            rc->loop_continue_handler_depth = saved_continue_handler_depth;
            break;
        }

        case STMT_LOOP: {
            size_t saved_break_count = rc->break_count;
            size_t saved_loop_start = rc->loop_start;
            int saved_loop_is_for = rc->loop_is_for;
            int saved_loop_depth = rc->loop_depth;
            uint8_t saved_break_reg = rc->loop_break_reg;
            uint8_t saved_continue_reg = rc->loop_continue_reg;
            size_t saved_break_lc = rc->loop_break_local_count;
            size_t saved_continue_lc = rc->loop_continue_local_count;
            int saved_break_scope_depth = rc->loop_break_scope_depth;
            int saved_continue_scope_depth = rc->loop_continue_scope_depth;
            int saved_break_handler_depth = rc->loop_break_handler_depth;
            int saved_continue_handler_depth = rc->loop_continue_handler_depth;

            rc->loop_break_local_count = rc->local_count;
            rc->loop_continue_local_count = rc->local_count;
            rc->loop_break_reg = rc->next_reg;
            rc->loop_continue_reg = rc->next_reg;
            rc->loop_break_scope_depth = rc->scope_depth;
            rc->loop_continue_scope_depth = rc->scope_depth;
            rc->loop_break_handler_depth = rc->handler_depth;
            rc->loop_continue_handler_depth = rc->handler_depth;
            rc->loop_start = current_chunk()->code_len;
            rc->loop_is_for = 0;
            rc->loop_depth++;

            begin_scope();
            for (size_t i = 0; i < s->as.loop.body_count; i++) compile_stmt(s->as.loop.body[i]);
            end_scope(line);

            emit_loop_back(rc->loop_start, line);

            for (size_t i = saved_break_count; i < rc->break_count; i++) patch_jmp(rc->break_patches[i]);
            rc->break_count = saved_break_count;
            rc->loop_start = saved_loop_start;
            rc->loop_is_for = saved_loop_is_for;
            rc->loop_depth = saved_loop_depth;
            rc->loop_break_reg = saved_break_reg;
            rc->loop_continue_reg = saved_continue_reg;
            rc->loop_break_local_count = saved_break_lc;
            rc->loop_continue_local_count = saved_continue_lc;
            rc->loop_break_scope_depth = saved_break_scope_depth;
            rc->loop_continue_scope_depth = saved_continue_scope_depth;
            rc->loop_break_handler_depth = saved_break_handler_depth;
            rc->loop_continue_handler_depth = saved_continue_handler_depth;
            break;
        }

        case STMT_FOR: {
            size_t saved_break_count = rc->break_count;
            size_t saved_continue_count = rc->continue_count;
            size_t saved_loop_start = rc->loop_start;
            int saved_loop_is_for = rc->loop_is_for;
            int saved_loop_depth = rc->loop_depth;
            uint8_t saved_break_reg = rc->loop_break_reg;
            uint8_t saved_continue_reg = rc->loop_continue_reg;
            size_t saved_break_lc = rc->loop_break_local_count;
            size_t saved_continue_lc = rc->loop_continue_local_count;
            int saved_break_scope_depth = rc->loop_break_scope_depth;
            int saved_continue_scope_depth = rc->loop_continue_scope_depth;
            int saved_break_handler_depth = rc->loop_break_handler_depth;
            int saved_continue_handler_depth = rc->loop_continue_handler_depth;

            rc->loop_break_local_count = rc->local_count;
            rc->loop_break_scope_depth = rc->scope_depth;
            rc->loop_break_handler_depth = rc->handler_depth;

            begin_scope();
            rc->loop_continue_scope_depth = rc->scope_depth;
            rc->loop_continue_handler_depth = rc->handler_depth;

            /* Compile iterator and init */
            uint8_t iter_reg = alloc_reg(); /* collection/range */
            compile_expr(s->as.for_loop.iter, iter_reg, line);
            uint8_t idx_reg = alloc_reg(); /* index counter */
            emit_ABC(ROP_ITERINIT, iter_reg, iter_reg, 0, line);
            emit_AsBx(ROP_LOADI, idx_reg, 0, line); /* idx = 0 */

            /* Hidden locals for iter state */
            {
                RegLocal *l1 = &rc->locals[rc->local_count++];
                l1->name = strdup("");
                l1->depth = rc->scope_depth;
                l1->is_captured = false;
                l1->reg = iter_reg;
                RegLocal *l2 = &rc->locals[rc->local_count++];
                l2->name = strdup("");
                l2->depth = rc->scope_depth;
                l2->is_captured = false;
                l2->reg = idx_reg;
            }

            rc->loop_continue_local_count = rc->local_count;
            rc->loop_continue_reg = rc->next_reg;
            rc->loop_start = current_chunk()->code_len;
            rc->loop_is_for = 1;
            rc->loop_depth++;

            /* Loop variable */
            uint8_t var_reg = add_local(s->as.for_loop.var);

            /* Emit length check: compare idx against collection length */
            uint8_t len_reg = alloc_reg();
            emit_ABC(ROP_LEN, len_reg, iter_reg, 0, line);
            uint8_t cmp_reg = alloc_reg();
            emit_ABC(ROP_LT_INT, cmp_reg, idx_reg, len_reg, line);
            size_t exit_jmp = emit_jump_placeholder(ROP_JMPFALSE, cmp_reg, line);
            free_reg(cmp_reg);
            free_reg(len_reg);

            /* Get current element */
            emit_ABC(ROP_ITERNEXT, var_reg, iter_reg, idx_reg, line);

            rc->loop_break_reg = rc->next_reg;

            /* Compile body */
            begin_scope();
            for (size_t i = 0; i < s->as.for_loop.body_count; i++) compile_stmt(s->as.for_loop.body[i]);
            end_scope(line);

            {
                int var_local = resolve_local(rc, s->as.for_loop.var);
                if (var_local >= 0 && rc->locals[var_local].is_captured)
                    emit_ABC(ROP_CLOSEUPVALUE, var_reg, 0, 0, line);
            }

            /* Increment index — continue forward-jumps land here.
             * Patch continue jumps before emitting INC so code_len == inc addr. */
            for (size_t i = saved_continue_count; i < rc->continue_count; i++) patch_jmp(rc->continue_patches[i]);
            rc->continue_count = saved_continue_count;
            emit_ABC(ROP_INC_REG, idx_reg, 0, 0, line);

            emit_loop_back(rc->loop_start, line);
            patch_jump(exit_jmp);

            end_scope(line);

            for (size_t i = saved_break_count; i < rc->break_count; i++) patch_jmp(rc->break_patches[i]);
            rc->break_count = saved_break_count;
            rc->loop_start = saved_loop_start;
            rc->loop_is_for = saved_loop_is_for;
            rc->loop_depth = saved_loop_depth;
            rc->loop_break_reg = saved_break_reg;
            rc->loop_continue_reg = saved_continue_reg;
            rc->loop_break_local_count = saved_break_lc;
            rc->loop_continue_local_count = saved_continue_lc;
            rc->loop_break_scope_depth = saved_break_scope_depth;
            rc->loop_continue_scope_depth = saved_continue_scope_depth;
            rc->loop_break_handler_depth = saved_break_handler_depth;
            rc->loop_continue_handler_depth = saved_continue_handler_depth;
            break;
        }

        case STMT_BREAK: {
            if (rc->loop_depth == 0) {
                rc_error = strdup("break outside of loop");
                return;
            }
            emit_loop_control_cleanup(rc->loop_break_local_count, rc->loop_break_scope_depth,
                                      rc->loop_break_handler_depth, line);
            size_t jmp = emit_jmp_placeholder(line);
            push_break_patch(jmp);
            break;
        }

        case STMT_CONTINUE: {
            if (rc->loop_depth == 0) {
                rc_error = strdup("continue outside of loop");
                return;
            }
            emit_loop_control_cleanup(rc->loop_continue_local_count, rc->loop_continue_scope_depth,
                                      rc->loop_continue_handler_depth, line);
            if (rc->loop_is_for) {
                /* For-loop: forward jump to increment, patched later */
                size_t jmp = emit_jmp_placeholder(line);
                push_continue_patch(jmp);
            } else {
                /* While/loop: backward jump to condition check */
                emit_loop_back(rc->loop_start, line);
            }
            break;
        }

        case STMT_DESTRUCTURE: {
            /* Compile source expression */
            uint8_t src_reg = alloc_reg();
            compile_expr(s->as.destructure.value, src_reg, line);

            if (s->as.destructure.kind == DESTRUCT_ARRAY) {
                /* Type and shape checks happen before any local/global is
                 * defined, so a failed destructure cannot leave partial state. */
                {
                    uint8_t type_ok = alloc_reg();
                    emit_ABC(ROP_MATCH_TYPE, type_ok, src_reg, VAL_ARRAY, line);
                    emit_assert_reg(type_ok, "cannot destructure value as array", line);
                    free_reg(type_ok);
                }
                {
                    uint8_t actual_len = alloc_reg();
                    uint8_t expected_len = alloc_reg();
                    uint8_t shape_ok = alloc_reg();
                    emit_ABC(ROP_LEN, actual_len, src_reg, 0, line);
                    emit_AsBx(ROP_LOADI, expected_len, (int16_t)s->as.destructure.name_count, line);
                    emit_ABC(s->as.destructure.rest_name ? ROP_GTEQ : ROP_EQ, shape_ok, actual_len, expected_len, line);
                    emit_assert_reg(shape_ok,
                                    s->as.destructure.rest_name ? "array destructure has too few elements"
                                                                : "array destructure length mismatch",
                                    line);
                    free_reg(shape_ok);
                    free_reg(expected_len);
                    free_reg(actual_len);
                }

                /* Array destructuring: let [a, b, c] = expr */
                for (size_t i = 0; i < s->as.destructure.name_count; i++) {
                    if (s->as.destructure.names[i][0] == '\0') continue; /* skip blank */
                    uint8_t idx_reg = alloc_reg();
                    emit_AsBx(ROP_LOADI, idx_reg, (int16_t)i, line);
                    if (rc->scope_depth > 0) {
                        uint8_t var_reg = add_local(s->as.destructure.names[i]);
                        emit_ABC(ROP_GETINDEX, var_reg, src_reg, idx_reg, line);
                        if (s->as.destructure.phase == PHASE_FLUID) emit_ABC(ROP_MARKFLUID, var_reg, 0, 0, line);
                        else if (s->as.destructure.phase == PHASE_CRYSTAL)
                            emit_ABC(ROP_FREEZE, var_reg, var_reg, 0, line);
                    } else {
                        uint8_t val_reg = alloc_reg();
                        emit_ABC(ROP_GETINDEX, val_reg, src_reg, idx_reg, line);
                        if (s->as.destructure.phase == PHASE_FLUID) emit_ABC(ROP_MARKFLUID, val_reg, 0, 0, line);
                        else if (s->as.destructure.phase == PHASE_CRYSTAL)
                            emit_ABC(ROP_FREEZE, val_reg, val_reg, 0, line);
                        uint16_t nki = add_constant(value_string(s->as.destructure.names[i]));
                        emit_ABx(ROP_DEFINEGLOBAL, val_reg, nki, line);
                        free_reg(val_reg);
                    }
                    free_reg(idx_reg);
                }
                /* Handle ...rest — slice tail of array using BUILDRANGE + GETINDEX */
                if (s->as.destructure.rest_name) {
                    uint8_t rest_start = alloc_reg();
                    emit_AsBx(ROP_LOADI, rest_start, (int16_t)s->as.destructure.name_count, line);
                    uint8_t rest_len = alloc_reg();
                    emit_ABC(ROP_LEN, rest_len, src_reg, 0, line);
                    uint8_t range_reg = alloc_reg();
                    emit_ABC(ROP_BUILDRANGE, range_reg, rest_start, rest_len, line);
                    if (rc->scope_depth > 0) {
                        uint8_t var_reg = add_local(s->as.destructure.rest_name);
                        emit_ABC(ROP_GETINDEX, var_reg, src_reg, range_reg, line);
                        if (s->as.destructure.phase == PHASE_FLUID) emit_ABC(ROP_MARKFLUID, var_reg, 0, 0, line);
                        else if (s->as.destructure.phase == PHASE_CRYSTAL)
                            emit_ABC(ROP_FREEZE, var_reg, var_reg, 0, line);
                    } else {
                        uint8_t val_reg = alloc_reg();
                        emit_ABC(ROP_GETINDEX, val_reg, src_reg, range_reg, line);
                        if (s->as.destructure.phase == PHASE_FLUID) emit_ABC(ROP_MARKFLUID, val_reg, 0, 0, line);
                        else if (s->as.destructure.phase == PHASE_CRYSTAL)
                            emit_ABC(ROP_FREEZE, val_reg, val_reg, 0, line);
                        uint16_t nki = add_constant(value_string(s->as.destructure.rest_name));
                        emit_ABx(ROP_DEFINEGLOBAL, val_reg, nki, line);
                        free_reg(val_reg);
                    }
                    free_reg(range_reg);
                    free_reg(rest_len);
                    free_reg(rest_start);
                }
            } else {
                /* Struct destructuring accepts only Struct and Map values. Check
                 * the source before creating any local or global bindings. */
                {
                    uint8_t type_ok = alloc_reg();
                    emit_ABC(ROP_MATCH_TYPE, type_ok, src_reg, VAL_STRUCT, line);
                    size_t valid_struct = emit_jump_placeholder(ROP_JMPTRUE, type_ok, line);
                    emit_ABC(ROP_MATCH_TYPE, type_ok, src_reg, VAL_MAP, line);
                    patch_jump(valid_struct);
                    emit_assert_reg(type_ok, "cannot destructure value as struct", line);
                    free_reg(type_ok);
                }

                /* Validate every requested field before defining any binding.
                 * MATCH_FIELD is a non-throwing probe for both Struct and Map;
                 * the second pass below performs the actual extraction. */
                for (size_t i = 0; i < s->as.destructure.name_count; i++) {
                    uint16_t field_ki = add_constant(value_string(s->as.destructure.names[i]));
                    uint8_t field_value = alloc_reg();
                    uint8_t present = alloc_reg();
                    emit_match_field(present, field_value, src_reg, field_ki, line);
                    emit_assert_reg(present, "destructure field not found", line);
                    free_reg(present);
                    free_reg(field_value);
                }

                /* Struct destructuring: let { x, y } = expr */
                for (size_t i = 0; i < s->as.destructure.name_count; i++) {
                    uint16_t field_ki = add_constant(value_string(s->as.destructure.names[i]));
                    if (rc->scope_depth > 0) {
                        uint8_t var_reg = add_local(s->as.destructure.names[i]);
                        emit_getfield(var_reg, src_reg, field_ki, line);
                        if (s->as.destructure.phase == PHASE_FLUID) emit_ABC(ROP_MARKFLUID, var_reg, 0, 0, line);
                        else if (s->as.destructure.phase == PHASE_CRYSTAL)
                            emit_ABC(ROP_FREEZE, var_reg, var_reg, 0, line);
                    } else {
                        uint8_t val_reg = alloc_reg();
                        emit_getfield(val_reg, src_reg, field_ki, line);
                        if (s->as.destructure.phase == PHASE_FLUID) emit_ABC(ROP_MARKFLUID, val_reg, 0, 0, line);
                        else if (s->as.destructure.phase == PHASE_CRYSTAL)
                            emit_ABC(ROP_FREEZE, val_reg, val_reg, 0, line);
                        uint16_t nki = add_constant(value_string(s->as.destructure.names[i]));
                        emit_ABx(ROP_DEFINEGLOBAL, val_reg, nki, line);
                        free_reg(val_reg);
                    }
                }
            }
            free_reg(src_reg);
            break;
        }

        case STMT_DEFER: {
            Expr block = {0};
            block.tag = EXPR_BLOCK;
            block.line = line;
            block.as.block.stmts = s->as.defer.body;
            block.as.block.count = s->as.defer.body_count;
            Expr closure_expr = {0};
            closure_expr.tag = EXPR_CLOSURE;
            closure_expr.line = line;
            closure_expr.as.closure.body = &block;

            uint8_t closure_reg = alloc_reg();
            compile_expr(&closure_expr, closure_reg, line);
            emit_ABC(ROP_DEFER_PUSH, closure_reg, (uint8_t)rc->scope_depth, 0, line);
            free_reg(closure_reg);
            break;
        }

        case STMT_IMPORT: {
            uint16_t path_ki = add_constant(value_string(s->as.import.module_path));

            if (s->as.import.alias) {
                if (rc->scope_depth > 0) {
                    uint8_t alias_reg = add_local(s->as.import.alias);
                    emit_ABx(ROP_IMPORT, alias_reg, path_ki, line);
                } else {
                    uint8_t tmp = alloc_reg();
                    emit_ABx(ROP_IMPORT, tmp, path_ki, line);
                    uint16_t alias_ki = add_constant(value_string(s->as.import.alias));
                    emit_ABx(ROP_DEFINEGLOBAL, tmp, alias_ki, line);
                    free_reg(tmp);
                }
            } else if (s->as.import.selective_names && s->as.import.selective_count > 0) {
                for (size_t i = 0; i < s->as.import.selective_count; i++)
                    rc_register_imported_enum_candidate(s->as.import.selective_names[i]);
                /* Keep the module in a hidden local while local exports are
                 * extracted; this keeps register allocation contiguous. */
                uint8_t tmp = rc->scope_depth > 0 ? add_local("") : alloc_reg();
                emit_ABx(ROP_IMPORT, tmp, path_ki, line);
                /* import { a, b } from "path" → extract each name */
                for (size_t i = 0; i < s->as.import.selective_count; i++) {
                    uint16_t field_ki = add_constant(value_string(s->as.import.selective_names[i]));
                    uint8_t val_reg = rc->scope_depth > 0 ? add_local(s->as.import.selective_names[i]) : alloc_reg();
                    emit_getfield(val_reg, tmp, field_ki, line);
                    /* Check that the export exists (not nil) */
                    size_t ok = emit_jump_placeholder(ROP_JMPNOTNIL, val_reg, line);
                    {
                        uint8_t err_reg = alloc_reg();
                        char errmsg[512];
                        snprintf(errmsg, sizeof(errmsg), "module '%s' does not export '%s'", s->as.import.module_path,
                                 s->as.import.selective_names[i]);
                        uint16_t err_ki = add_constant(value_string(errmsg));
                        emit_ABx(ROP_LOADK, err_reg, err_ki, line);
                        emit_ABC(ROP_THROW, err_reg, 0, 0, line);
                        free_reg(err_reg);
                    }
                    patch_jump(ok);
                    if (rc->scope_depth == 0) {
                        uint16_t name_ki = add_constant(value_string(s->as.import.selective_names[i]));
                        emit_ABx(ROP_DEFINEGLOBAL, val_reg, name_ki, line);
                        free_reg(val_reg);
                    }
                }
                if (rc->scope_depth == 0) free_reg(tmp);
            } else {
                uint8_t tmp = alloc_reg();
                emit_ABx(ROP_IMPORT, tmp, path_ki, line);
                free_reg(tmp);
            }
            break;
        }

        default: break;
    }
}

/* Emit ensure (postcondition) checks and return type checks before return */
static void emit_postconditions(uint8_t reg, int line) {
    /* Return type check */
    if (rc && rc->return_type) {
        uint16_t type_ki = add_constant(value_string(rc->return_type));
        emit_ABx(ROP_CHECK_TYPE, reg, type_ki, line);
        /* Second word: error message constant index */
        char err_msg[512];
        snprintf(err_msg, sizeof(err_msg), "return type expects %s, got %%s", rc->return_type);
        uint32_t err_ki = (uint32_t)add_constant(value_string(err_msg));
        emit(err_ki, line);
    }
    if (!rc || !rc->ensures) return;
    for (size_t ei = 0; ei < rc->ensure_count; ei++) {
        if (!rc->ensures[ei].is_ensure) continue;
        uint8_t fn_reg = alloc_reg();
        uint8_t arg_reg = alloc_reg(); /* Must be fn_reg+1 */
        compile_expr(rc->ensures[ei].condition, fn_reg, line);
        emit_ABC(ROP_MOVE, arg_reg, reg, 0, line);
        emit_ABC(ROP_CALL, fn_reg, 1, 1, line);
        /* fn_reg now has the result */
        size_t ok = emit_jump_placeholder(ROP_JMPTRUE, fn_reg, line);
        uint8_t err_reg = alloc_reg();
        const char *user_msg = rc->ensures[ei].message ? rc->ensures[ei].message : "postcondition not met";
        char full_msg[512];
        snprintf(full_msg, sizeof(full_msg), "ensure failed in '%s': %s", rc->func_name ? rc->func_name : "<anonymous>",
                 user_msg);
        uint16_t msg_ki = add_constant(value_string(full_msg));
        emit_ABx(ROP_LOADK, err_reg, msg_ki, line);
        emit_ABC(ROP_THROW, err_reg, 0, 0, line);
        free_reg(err_reg);
        patch_jump(ok);
        free_reg(arg_reg);
        free_reg(fn_reg);
    }
}

/* Compile an impl method with the same semantic pipeline as an ordinary
 * function. The receiver occupies R0; explicit arguments start at R1. */
static void compile_impl_method(const char *type_name, FnDecl *method, int line) {
    char key[256];
    snprintf(key, sizeof(key), "%s::%s", type_name, method->name);

    RegCompiler func_comp;
    rc_init(&func_comp, rc, REG_FUNC_FUNCTION);
    func_comp.func_name = strdup(key);
    func_comp.chunk->name = strdup(key);
    if (method->return_type && method->return_type->name) func_comp.return_type = method->return_type->name;

    bool has_self = method->param_count > 0 && strcmp(method->params[0].name, "self") == 0;
    size_t first_param = has_self ? 1 : 0;
    if (has_self) {
        free(func_comp.locals[0].name);
        func_comp.locals[0].name = strdup("self");
        regchunk_set_local_name(func_comp.chunk, 0, "self");
    }

    size_t declared_arity = method->param_count - first_param;
    bool has_variadic = false;
    for (size_t k = first_param; k < method->param_count; k++) {
        if (method->params[k].is_variadic) {
            declared_arity = k - first_param;
            has_variadic = true;
            break;
        }
    }
    func_comp.arity = (int)declared_arity;

    for (size_t k = first_param; k < method->param_count; k++) add_local(method->params[k].name);

    func_comp.chunk->default_count = 0;
    for (size_t k = first_param; k < method->param_count; k++) {
        if (!method->params[k].default_value || method->params[k].is_variadic) continue;
        func_comp.chunk->default_count++;
        uint8_t preg = local_reg((int)(k - first_param + 1));
        size_t skip = emit_default_skip(preg, line);
        compile_expr(method->params[k].default_value, preg, line);
        patch_jump(skip);
    }

    if (has_variadic) {
        uint8_t var_reg = local_reg((int)(declared_arity + 1));
        emit_ABC(ROP_COLLECT_VARARGS, var_reg, (uint8_t)(declared_arity + 1), 0, line);
    }

    /* Runtime parameter type checks, including an explicitly typed self. */
    for (size_t k = 0; k < method->param_count; k++) {
        if (method->params[k].is_variadic) break;
        if (!method->params[k].ty.name || strcmp(method->params[k].ty.name, "Any") == 0 ||
            strcmp(method->params[k].ty.name, "any") == 0)
            continue;
        uint8_t preg = has_self && k == 0 ? 0 : local_reg((int)(k - first_param + 1));
        uint16_t type_ki = add_constant(value_string(method->params[k].ty.name));
        emit_ABx(ROP_CHECK_TYPE, preg, type_ki, line);
        char err_msg[512];
        snprintf(err_msg, sizeof(err_msg), "function '%s' parameter '%s' expects type %s, got %%s", key,
                 method->params[k].name, method->params[k].ty.name);
        emit((uint32_t)add_constant(value_string(err_msg)), line);
    }

    for (size_t ci = 0; ci < method->contract_count; ci++) {
        if (method->contracts[ci].is_ensure) continue;
        uint8_t cond_reg = alloc_reg();
        compile_expr(method->contracts[ci].condition, cond_reg, line);
        size_t ok = emit_jump_placeholder(ROP_JMPTRUE, cond_reg, line);
        uint8_t err_reg = alloc_reg();
        const char *msg = method->contracts[ci].message ? method->contracts[ci].message : "condition not met";
        char full_msg[512];
        snprintf(full_msg, sizeof(full_msg), "require failed in '%s': %s", key, msg);
        emit_ABx(ROP_LOADK, err_reg, add_constant(value_string(full_msg)), line);
        emit_ABC(ROP_THROW, err_reg, 0, 0, line);
        free_reg(err_reg);
        patch_jump(ok);
        free_reg(cond_reg);
    }

    func_comp.ensures = method->contracts;
    func_comp.ensure_count = method->contract_count;

    if (method->body_count > 0 && method->body[method->body_count - 1]->tag == STMT_EXPR) {
        for (size_t k = 0; k + 1 < method->body_count; k++) compile_stmt(method->body[k]);
        uint8_t ret_reg = alloc_reg();
        compile_expr(method->body[method->body_count - 1]->as.expr, ret_reg, line);
        emit_postconditions(ret_reg, line);
        emit_return(ret_reg, line);
        free_reg(ret_reg);
    } else {
        for (size_t k = 0; k < method->body_count; k++) compile_stmt(method->body[k]);
        uint8_t ret_reg = alloc_reg();
        emit_ABC(ROP_LOADUNIT, ret_reg, 0, 0, line);
        emit_postconditions(ret_reg, line);
        emit_return(ret_reg, line);
        free_reg(ret_reg);
    }

    RegChunk *fn_chunk = func_comp.chunk;

    /* Impl frames receive only the explicit arguments in R1+, with self in
     * R0. Keep phase metadata indexed to those explicit arguments so every
     * direct method-dispatch path can validate it before entering the frame. */
    {
        bool has_phase_constraints = false;
        size_t checked_param_count = 0;
        for (size_t k = first_param; k < method->param_count; k++) {
            if (method->params[k].is_variadic) break;
            checked_param_count++;
            if (method->params[k].ty.phase != PHASE_UNSPECIFIED) { has_phase_constraints = true; }
        }
        if (has_phase_constraints) {
            fn_chunk->param_phases = calloc(checked_param_count, sizeof(uint8_t));
            if (!fn_chunk->param_phases) {
                rc_error = strdup("out of memory compiling impl parameter phases");
                return;
            }
            fn_chunk->param_phase_count = (int)checked_param_count;
            for (size_t k = first_param; k < method->param_count; k++) {
                if (method->params[k].is_variadic) break;
                fn_chunk->param_phases[k - first_param] = (uint8_t)method->params[k].ty.phase;
            }
        }
    }

    size_t upvalue_count = func_comp.upvalue_count;
    RegCompilerUpvalue *upvalues = NULL;
    if (upvalue_count > 0) {
        upvalues = malloc(upvalue_count * sizeof(RegCompilerUpvalue));
        if (!upvalues) {
            rc_error = strdup("out of memory compiling impl method");
            return;
        }
        memcpy(upvalues, func_comp.upvalues, upvalue_count * sizeof(RegCompilerUpvalue));
    }
    rc_cleanup(&func_comp);
    rc = func_comp.enclosing;

    LatValue fn_val;
    memset(&fn_val, 0, sizeof(fn_val));
    fn_val.type = VAL_CLOSURE;
    fn_val.phase = VTAG_UNPHASED;
    fn_val.region_id = REGION_NONE;
    fn_val.as.closure.upvalue_count = (uint32_t)upvalue_count;
    fn_val.as.closure.native_fn = fn_chunk;
    fn_val.as.closure.param_count = method->param_count;
    fn_val.as.closure.has_variadic = has_variadic;
    if (method->param_count > 0) {
        fn_val.as.closure.param_names = malloc(method->param_count * sizeof(char *));
        if (!fn_val.as.closure.param_names) {
            free(upvalues);
            rc_error = strdup("out of memory compiling impl method");
            return;
        }
        for (size_t k = 0; k < method->param_count; k++)
            fn_val.as.closure.param_names[k] = strdup(method->params[k].name);
    }

    uint16_t fn_ki = add_constant(fn_val);
    uint8_t dst = alloc_reg();
    emit_ABx(ROP_CLOSURE, dst, fn_ki, line);
    for (size_t k = 0; k < upvalue_count; k++)
        emit_ABC(ROP_MOVE, upvalues[k].is_local ? 1 : 0, upvalues[k].index, 0, line);
    free(upvalues);
    emit_ABx(ROP_DEFINEGLOBAL, dst, add_constant(value_string(key)), line);
    free_reg(dst);
}

/* ── Function body compilation ── */

static void compile_function_body(RegFuncType type, const char *name, Param *params, size_t param_count, Stmt **body,
                                  size_t body_count, int line, ContractClause *contracts, size_t contract_count,
                                  TypeExpr *return_type) {
    RegCompiler func_comp;
    rc_init(&func_comp, rc, type);
    func_comp.func_name = name ? strdup(name) : NULL;
    func_comp.chunk->name = name ? strdup(name) : NULL;
    if (return_type && return_type->name) func_comp.return_type = return_type->name;

    /* Count non-variadic params for arity */
    size_t declared_arity = param_count;
    bool has_variadic = false;
    for (size_t i = 0; i < param_count; i++) {
        if (params[i].is_variadic) {
            declared_arity = i;
            has_variadic = true;
            break;
        }
    }
    func_comp.arity = (int)declared_arity;
    func_comp.chunk->default_count = 0;
    for (size_t i = 0; i < declared_arity; i++)
        if (params[i].default_value) func_comp.chunk->default_count++;

    /* Add params as locals (they occupy R1..Rn) */
    for (size_t i = 0; i < param_count; i++) add_local(params[i].name);

    /* Emit default parameter initialization */
    for (size_t i = 0; i < param_count; i++) {
        if (params[i].default_value && !params[i].is_variadic) {
            /* If param is nil, use default value */
            uint8_t preg = local_reg((int)(i + 1)); /* +1 because slot 0 is reserved */
            size_t skip = emit_default_skip(preg, line);
            compile_expr(params[i].default_value, preg, line);
            patch_jump(skip);
        }
    }

    /* Emit variadic collection if needed */
    if (has_variadic) {
        uint8_t var_reg = local_reg((int)(declared_arity + 1));
        emit_ABC(ROP_COLLECT_VARARGS, var_reg, (uint8_t)(declared_arity + 1), 0, line);
    }

    /* Emit runtime parameter type checks */
    for (size_t i = 0; i < param_count; i++) {
        if (params[i].is_variadic) break;
        if (!params[i].ty.name || strcmp(params[i].ty.name, "Any") == 0 || strcmp(params[i].ty.name, "any") == 0)
            continue;
        uint8_t preg = local_reg((int)(i + 1));
        uint16_t type_ki = add_constant(value_string(params[i].ty.name));
        emit_ABx(ROP_CHECK_TYPE, preg, type_ki, line);
        char err_msg[512];
        snprintf(err_msg, sizeof(err_msg), "function '%s' parameter '%s' expects type %s, got %%s",
                 name ? name : "<anonymous>", params[i].name, params[i].ty.name);
        uint32_t err_ki = (uint32_t)add_constant(value_string(err_msg));
        emit(err_ki, line);
    }

    /* Emit require contracts (preconditions) */
    if (contracts) {
        for (size_t ci = 0; ci < contract_count; ci++) {
            if (contracts[ci].is_ensure) continue;
            uint8_t cond_reg = alloc_reg();
            compile_expr(contracts[ci].condition, cond_reg, line);
            size_t ok = emit_jump_placeholder(ROP_JMPTRUE, cond_reg, line);
            /* Condition is false — throw error */
            const char *user_msg = contracts[ci].message ? contracts[ci].message : "condition not met";
            char full_msg[512];
            snprintf(full_msg, sizeof(full_msg), "require failed in '%s': %s", name ? name : "<anonymous>", user_msg);
            uint8_t err_reg = alloc_reg();
            uint16_t msg_ki = add_constant(value_string(full_msg));
            emit_ABx(ROP_LOADK, err_reg, msg_ki, line);
            emit_ABC(ROP_THROW, err_reg, 0, 0, line);
            free_reg(err_reg);
            patch_jump(ok);
            free_reg(cond_reg);
        }
    }

    /* Set ensure contracts and return type on compiler state for emit_return */
    if (contracts) {
        size_t ec = 0;
        for (size_t ci = 0; ci < contract_count; ci++)
            if (contracts[ci].is_ensure) ec++;
        if (ec > 0) {
            rc->ensures = contracts;
            rc->ensure_count = contract_count; /* emit_return filters by is_ensure */
        }
    }

    /* Compile body — last expression is implicit return value */
    if (body_count > 0 && body[body_count - 1]->tag == STMT_EXPR) {
        for (size_t i = 0; i + 1 < body_count; i++) compile_stmt(body[i]);
        uint8_t ret_reg = alloc_reg();
        compile_expr(body[body_count - 1]->as.expr, ret_reg, line);
        emit_postconditions(ret_reg, line);
        emit_return(ret_reg, line);
        free_reg(ret_reg);
    } else {
        for (size_t i = 0; i < body_count; i++) compile_stmt(body[i]);
        /* Implicit unit return */
        uint8_t ret_reg = alloc_reg();
        emit_ABC(ROP_LOADUNIT, ret_reg, 0, 0, line);
        emit_postconditions(ret_reg, line);
        emit_return(ret_reg, line);
        free_reg(ret_reg);
    }

    RegChunk *fn_chunk = func_comp.chunk;

    /* Store parameter phase constraints on chunk for runtime checking */
    {
        bool has_phase_constraints = false;
        for (size_t i = 0; i < param_count; i++) {
            if (params[i].is_variadic) break;
            if (params[i].ty.phase != PHASE_UNSPECIFIED) {
                has_phase_constraints = true;
                break;
            }
        }
        if (has_phase_constraints) {
            fn_chunk->param_phases = calloc(param_count, sizeof(uint8_t));
            if (!fn_chunk->param_phases) return;
            fn_chunk->param_phase_count = (int)param_count;
            for (size_t i = 0; i < param_count; i++) {
                if (params[i].is_variadic) break;
                fn_chunk->param_phases[i] = (uint8_t)params[i].ty.phase;
            }
        }
    }

    size_t upvalue_count = func_comp.upvalue_count;
    RegCompilerUpvalue *upvalues = NULL;
    if (upvalue_count > 0) {
        upvalues = malloc(upvalue_count * sizeof(RegCompilerUpvalue));
        if (!upvalues) return;
        memcpy(upvalues, func_comp.upvalues, upvalue_count * sizeof(RegCompilerUpvalue));
    }
    rc_cleanup(&func_comp);
    rc = func_comp.enclosing;

    /* Store as a closure constant */
    LatValue fn_val;
    memset(&fn_val, 0, sizeof(fn_val));
    fn_val.type = VAL_CLOSURE;
    fn_val.phase = VTAG_UNPHASED;
    fn_val.region_id = REGION_NONE;
    fn_val.as.closure.upvalue_count = (uint32_t)upvalue_count;
    fn_val.as.closure.body = NULL;
    fn_val.as.closure.native_fn = fn_chunk;
    fn_val.as.closure.param_count = param_count;
    fn_val.as.closure.has_variadic = has_variadic;
    if (param_count > 0 && params) {
        fn_val.as.closure.param_names = malloc(param_count * sizeof(char *));
        if (!fn_val.as.closure.param_names) return;
        for (size_t pi = 0; pi < param_count; pi++) fn_val.as.closure.param_names[pi] = strdup(params[pi].name);
    }
    uint16_t fn_ki = add_constant(fn_val);

    uint8_t dst = alloc_reg();
    emit_ABx(ROP_CLOSURE, dst, fn_ki, line);
    for (size_t i = 0; i < upvalue_count; i++) {
        emit_ABC(ROP_MOVE, upvalues[i].is_local ? 1 : 0, upvalues[i].index, 0, line);
    }
    free(upvalues);

    /* Define as global */
    uint16_t name_ki = add_constant(value_string(name));
    emit_ABx(ROP_DEFINEGLOBAL, dst, name_ki, line);
    free_reg(dst);
}

/* ── Top-level compilation ── */

RegChunk *reg_compile(const Program *prog, char **error) {
    RegCompiler top;
    rc_init(&top, NULL, REG_FUNC_SCRIPT);
    *error = NULL;

    for (size_t i = 0; i < prog->item_count; i++) {
        if (rc_error) break;

        switch (prog->items[i].tag) {
            case ITEM_STMT: compile_stmt(prog->items[i].as.stmt); break;

            case ITEM_FUNCTION: {
                FnDecl *fn = &prog->items[i].as.fn_decl;
                compile_function_body(REG_FUNC_FUNCTION, fn->name, fn->params, fn->param_count, fn->body,
                                      fn->body_count, 0, fn->contracts, fn->contract_count, fn->return_type);
                break;
            }

            case ITEM_STRUCT: {
                StructDecl *sd = &prog->items[i].as.struct_decl;
                /* Store struct metadata as "__struct_<name>" global */
                LatValue *field_names = malloc(sd->field_count * sizeof(LatValue));
                if (!field_names) return NULL;
                for (size_t j = 0; j < sd->field_count; j++) field_names[j] = value_string(sd->fields[j].name);
                LatValue arr = value_array(field_names, sd->field_count);
                free(field_names);

                char meta_name[256];
                snprintf(meta_name, sizeof(meta_name), "__struct_%s", sd->name);
                uint8_t tmp = alloc_reg();
                uint16_t arr_ki = add_constant(arr);
                emit_ABx(ROP_LOADK, tmp, arr_ki, 0);
                uint16_t name_ki = add_constant(value_string(meta_name));
                emit_ABx(ROP_DEFINEGLOBAL, tmp, name_ki, 0);
                free_reg(tmp);

                /* Alloy: emit per-field phase metadata if any field has a phase annotation */
                {
                    bool has_phase_ann = false;
                    for (size_t j = 0; j < sd->field_count; j++) {
                        if (sd->fields[j].ty.phase != PHASE_UNSPECIFIED) {
                            has_phase_ann = true;
                            break;
                        }
                    }
                    if (has_phase_ann) {
                        LatValue *phases = malloc(sd->field_count * sizeof(LatValue));
                        if (!phases) return NULL;
                        for (size_t j = 0; j < sd->field_count; j++)
                            phases[j] = value_int((int64_t)sd->fields[j].ty.phase);
                        LatValue phase_arr = value_array(phases, sd->field_count);
                        free(phases);
                        char phase_meta[256];
                        snprintf(phase_meta, sizeof(phase_meta), "__struct_phases_%s", sd->name);
                        uint8_t pt = alloc_reg();
                        uint16_t pi = add_constant(phase_arr);
                        emit_ABx(ROP_LOADK, pt, pi, 0);
                        uint16_t pn = add_constant(value_string(phase_meta));
                        emit_ABx(ROP_DEFINEGLOBAL, pt, pn, 0);
                        free_reg(pt);
                    }
                }
                break;
            }

            case ITEM_ENUM: {
                EnumDecl *ed = &prog->items[i].as.enum_decl;
                rc_register_enum(ed);
                char meta_name[256];
                snprintf(meta_name, sizeof(meta_name), "__enum_%s", ed->name);
                uint8_t tmp = alloc_reg();
                emit_ABx(ROP_LOADK, tmp, add_constant(rc_enum_metadata_value(ed)), 0);
                uint16_t name_ki = add_constant(value_string(meta_name));
                emit_ABx(ROP_DEFINEGLOBAL, tmp, name_ki, 0);
                free_reg(tmp);
                break;
            }

            case ITEM_IMPL: {
                ImplBlock *ib = &prog->items[i].as.impl_block;
                for (size_t j = 0; j < ib->method_count; j++)
                    compile_impl_method(ib->type_name, &ib->methods[j], ib->methods[j].line);
                break;

                /* Legacy inline lowering retained below temporarily for source
                 * compatibility; the shared helper above is authoritative. */
                for (size_t j = 0; j < ib->method_count; j++) {
                    FnDecl *method = &ib->methods[j];
                    char key[256];
                    snprintf(key, sizeof(key), "%s::%s", ib->type_name, method->name);

                    /* Compile method body */
                    RegCompiler func_comp;
                    rc_init(&func_comp, rc, REG_FUNC_FUNCTION);
                    func_comp.func_name = strdup(key);
                    func_comp.chunk->name = strdup(key);

                    /* self is slot 0 */
                    size_t first_param = 0;
                    if (method->param_count > 0 && strcmp(method->params[0].name, "self") == 0) {
                        free(func_comp.locals[0].name);
                        func_comp.locals[0].name = strdup("self");
                        first_param = 1;
                    }

                    /* Count non-variadic params for arity */
                    size_t m_declared_arity = method->param_count - first_param;
                    bool m_has_variadic = false;
                    for (size_t k = first_param; k < method->param_count; k++) {
                        if (method->params[k].is_variadic) {
                            m_declared_arity = k - first_param;
                            m_has_variadic = true;
                            break;
                        }
                    }
                    func_comp.arity = (int)m_declared_arity;
                    for (size_t k = first_param; k < method->param_count; k++) add_local(method->params[k].name);

                    /* Emit default parameter initialization.
                     * With self: locals[0]=self, locals[1..]=params[1..].
                     * Without self (shouldn't happen for impl): locals[0]="", locals[1..]=params[0..].
                     * In both cases, params[k] is at locals[k]. */
                    for (size_t k = first_param; k < method->param_count; k++) {
                        if (method->params[k].default_value && !method->params[k].is_variadic) {
                            uint8_t preg = local_reg((int)k);
                            size_t skip = emit_default_skip(preg, 0);
                            compile_expr(method->params[k].default_value, preg, 0);
                            patch_jump(skip);
                        }
                    }

                    /* Emit variadic collection if needed */
                    if (m_has_variadic) {
                        size_t var_idx = first_param + m_declared_arity;
                        uint8_t var_reg = local_reg((int)var_idx);
                        emit_ABC(ROP_COLLECT_VARARGS, var_reg, (uint8_t)(m_declared_arity + 1), 0, 0);
                    }

                    for (size_t k = 0; k < method->body_count; k++) compile_stmt(method->body[k]);

                    uint8_t ret_reg = alloc_reg();
                    emit_ABC(ROP_LOADUNIT, ret_reg, 0, 0, 0);
                    emit_return(ret_reg, 0);
                    free_reg(ret_reg);

                    RegChunk *fn_chunk = func_comp.chunk;
                    fn_chunk->default_count = 0;
                    for (size_t k = first_param; k < method->param_count; k++)
                        if (method->params[k].default_value) fn_chunk->default_count++;
                    size_t upvalue_count = func_comp.upvalue_count;
                    RegCompilerUpvalue *upvalues = NULL;
                    if (upvalue_count > 0) {
                        upvalues = malloc(upvalue_count * sizeof(RegCompilerUpvalue));
                        if (!upvalues) return NULL;
                        memcpy(upvalues, func_comp.upvalues, upvalue_count * sizeof(RegCompilerUpvalue));
                    }
                    rc_cleanup(&func_comp);
                    rc = func_comp.enclosing;

                    LatValue fn_val;
                    memset(&fn_val, 0, sizeof(fn_val));
                    fn_val.type = VAL_CLOSURE;
                    fn_val.phase = VTAG_UNPHASED;
                    fn_val.region_id = REGION_NONE;
                    fn_val.as.closure.upvalue_count = (uint32_t)upvalue_count;
                    fn_val.as.closure.body = NULL;
                    fn_val.as.closure.native_fn = fn_chunk;
                    fn_val.as.closure.param_count = method->param_count;
                    fn_val.as.closure.has_variadic = m_has_variadic;
                    if (method->param_count > 0 && method->params) {
                        fn_val.as.closure.param_names = malloc(method->param_count * sizeof(char *));
                        if (!fn_val.as.closure.param_names) return NULL;
                        for (size_t pi = 0; pi < method->param_count; pi++)
                            fn_val.as.closure.param_names[pi] = strdup(method->params[pi].name);
                    }
                    uint16_t fn_ki = add_constant(fn_val);

                    uint8_t dst = alloc_reg();
                    emit_ABx(ROP_CLOSURE, dst, fn_ki, 0);
                    for (size_t k = 0; k < upvalue_count; k++)
                        emit_ABC(ROP_MOVE, upvalues[k].is_local ? 1 : 0, upvalues[k].index, 0, 0);
                    free(upvalues);

                    uint16_t key_ki = add_constant(value_string(key));
                    emit_ABx(ROP_DEFINEGLOBAL, dst, key_ki, 0);
                    free_reg(dst);
                }
                break;
            }

            case ITEM_TRAIT:
            case ITEM_TEST: break;
        }
    }

    if (rc_error) {
        *error = rc_error;
        rc_error = NULL;
        regchunk_free(top.chunk);
        rc_cleanup(&top);
        rc = NULL;
        rc_free_known_enums();
        return NULL;
    }

    /* Auto-call main() if defined */
    bool has_main = false;
    for (size_t i = 0; i < prog->item_count; i++) {
        if (prog->items[i].tag == ITEM_FUNCTION && strcmp(prog->items[i].as.fn_decl.name, "main") == 0) {
            has_main = true;
            break;
        }
    }
    if (has_main) {
        uint8_t func_reg = alloc_reg();
        uint16_t main_ki = add_constant(value_string("main"));
        emit_ABx(ROP_GETGLOBAL, func_reg, main_ki, 0);
        emit_ABC(ROP_CALL, func_reg, 0, 1, 0);
        free_reg(func_reg);
    }

    /* Final halt + return */
    uint8_t ret_reg = alloc_reg();
    emit_ABC(ROP_LOADUNIT, ret_reg, 0, 0, 0);
    emit_return(ret_reg, 0);
    free_reg(ret_reg);

    RegChunk *result = top.chunk;
    rc_cleanup(&top);
    rc = NULL;
    return result;
}

/* Compile for REPL — keeps the last expression value in a known register */
static RegChunk *reg_compile_internal(const Program *prog, char **error, bool auto_main, bool keep_last_expr) {
    RegCompiler top;
    rc_init(&top, NULL, REG_FUNC_SCRIPT);
    *error = NULL;

    for (size_t i = 0; i < prog->item_count; i++) {
        if (rc_error) break;

        switch (prog->items[i].tag) {
            case ITEM_STMT:
                /* For REPL mode, if this is the last item and it's an expression,
                 * keep the result instead of discarding */
                if (keep_last_expr && i + 1 == prog->item_count && prog->items[i].as.stmt->tag == STMT_EXPR) {
                    uint8_t result_reg = alloc_reg();
                    compile_expr(prog->items[i].as.stmt->as.expr, result_reg, prog->items[i].as.stmt->line);
                    emit_return(result_reg, 0);
                    free_reg(result_reg);
                    goto finish;
                }
                compile_stmt(prog->items[i].as.stmt);
                break;

            case ITEM_FUNCTION: {
                FnDecl *fn = &prog->items[i].as.fn_decl;
                compile_function_body(REG_FUNC_FUNCTION, fn->name, fn->params, fn->param_count, fn->body,
                                      fn->body_count, 0, fn->contracts, fn->contract_count, fn->return_type);
                break;
            }

            case ITEM_STRUCT: {
                StructDecl *sd = &prog->items[i].as.struct_decl;
                LatValue *field_names = malloc(sd->field_count * sizeof(LatValue));
                if (!field_names) return NULL;
                for (size_t j = 0; j < sd->field_count; j++) field_names[j] = value_string(sd->fields[j].name);
                LatValue arr = value_array(field_names, sd->field_count);
                free(field_names);
                char meta_name[256];
                snprintf(meta_name, sizeof(meta_name), "__struct_%s", sd->name);
                uint8_t tmp = alloc_reg();
                uint16_t arr_ki = add_constant(arr);
                emit_ABx(ROP_LOADK, tmp, arr_ki, 0);
                uint16_t name_ki = add_constant(value_string(meta_name));
                emit_ABx(ROP_DEFINEGLOBAL, tmp, name_ki, 0);
                free_reg(tmp);
                /* Alloy: per-field phase metadata */
                {
                    bool has_phase_ann = false;
                    for (size_t j = 0; j < sd->field_count; j++) {
                        if (sd->fields[j].ty.phase != PHASE_UNSPECIFIED) {
                            has_phase_ann = true;
                            break;
                        }
                    }
                    if (has_phase_ann) {
                        LatValue *phases = malloc(sd->field_count * sizeof(LatValue));
                        if (!phases) return NULL;
                        for (size_t j = 0; j < sd->field_count; j++)
                            phases[j] = value_int((int64_t)sd->fields[j].ty.phase);
                        LatValue phase_arr = value_array(phases, sd->field_count);
                        free(phases);
                        char phase_meta[256];
                        snprintf(phase_meta, sizeof(phase_meta), "__struct_phases_%s", sd->name);
                        uint8_t pt = alloc_reg();
                        uint16_t pi = add_constant(phase_arr);
                        emit_ABx(ROP_LOADK, pt, pi, 0);
                        uint16_t pn = add_constant(value_string(phase_meta));
                        emit_ABx(ROP_DEFINEGLOBAL, pt, pn, 0);
                        free_reg(pt);
                    }
                }
                break;
            }

            case ITEM_ENUM: {
                EnumDecl *ed = &prog->items[i].as.enum_decl;
                rc_register_enum(ed);
                char meta_name[256];
                snprintf(meta_name, sizeof(meta_name), "__enum_%s", ed->name);
                uint8_t tmp = alloc_reg();
                emit_ABx(ROP_LOADK, tmp, add_constant(rc_enum_metadata_value(ed)), 0);
                uint16_t nk = add_constant(value_string(meta_name));
                emit_ABx(ROP_DEFINEGLOBAL, tmp, nk, 0);
                free_reg(tmp);
                break;
            }

            case ITEM_IMPL: {
                ImplBlock *ib = &prog->items[i].as.impl_block;
                for (size_t j = 0; j < ib->method_count; j++)
                    compile_impl_method(ib->type_name, &ib->methods[j], ib->methods[j].line);
                break;

                for (size_t j = 0; j < ib->method_count; j++) {
                    FnDecl *method = &ib->methods[j];
                    char key[256];
                    snprintf(key, sizeof(key), "%s::%s", ib->type_name, method->name);
                    RegCompiler func_comp;
                    rc_init(&func_comp, rc, REG_FUNC_FUNCTION);
                    func_comp.func_name = strdup(key);
                    func_comp.chunk->name = strdup(key);
                    size_t first_param = 0;
                    if (method->param_count > 0 && strcmp(method->params[0].name, "self") == 0) {
                        free(func_comp.locals[0].name);
                        func_comp.locals[0].name = strdup("self");
                        first_param = 1;
                    }
                    func_comp.arity = (int)(method->param_count - first_param);
                    for (size_t k = first_param; k < method->param_count; k++) add_local(method->params[k].name);
                    for (size_t k = 0; k < method->body_count; k++) compile_stmt(method->body[k]);
                    uint8_t rr = alloc_reg();
                    emit_ABC(ROP_LOADUNIT, rr, 0, 0, 0);
                    emit_return(rr, 0);
                    free_reg(rr);
                    RegChunk *fn_chunk = func_comp.chunk;
                    size_t uvc = func_comp.upvalue_count;
                    RegCompilerUpvalue *uvs = NULL;
                    if (uvc > 0) {
                        uvs = malloc(uvc * sizeof(RegCompilerUpvalue));
                        if (!uvs) return NULL;
                        memcpy(uvs, func_comp.upvalues, uvc * sizeof(RegCompilerUpvalue));
                    }
                    rc_cleanup(&func_comp);
                    rc = func_comp.enclosing;
                    LatValue fn_val;
                    memset(&fn_val, 0, sizeof(fn_val));
                    fn_val.type = VAL_CLOSURE;
                    fn_val.phase = VTAG_UNPHASED;
                    fn_val.region_id = REGION_NONE;
                    fn_val.as.closure.upvalue_count = (uint32_t)uvc;
                    fn_val.as.closure.body = NULL;
                    fn_val.as.closure.native_fn = fn_chunk;
                    fn_val.as.closure.param_count = method->param_count;
                    if (method->param_count > 0 && method->params) {
                        fn_val.as.closure.param_names = malloc(method->param_count * sizeof(char *));
                        if (!fn_val.as.closure.param_names) return NULL;
                        for (size_t pi = 0; pi < method->param_count; pi++)
                            fn_val.as.closure.param_names[pi] = strdup(method->params[pi].name);
                    }
                    uint16_t fn_ki = add_constant(fn_val);
                    uint8_t dst = alloc_reg();
                    emit_ABx(ROP_CLOSURE, dst, fn_ki, 0);
                    for (size_t k = 0; k < uvc; k++) emit_ABC(ROP_MOVE, uvs[k].is_local ? 1 : 0, uvs[k].index, 0, 0);
                    free(uvs);
                    uint16_t key_ki = add_constant(value_string(key));
                    emit_ABx(ROP_DEFINEGLOBAL, dst, key_ki, 0);
                    free_reg(dst);
                }
                break;
            }

            case ITEM_TRAIT:
            case ITEM_TEST: break;
        }
    }

    if (rc_error) {
        *error = rc_error;
        rc_error = NULL;
        regchunk_free(top.chunk);
        rc_cleanup(&top);
        rc = NULL;
        return NULL;
    }

    /* Auto-call main() if requested */
    if (auto_main) {
        bool has_main = false;
        for (size_t i = 0; i < prog->item_count; i++) {
            if (prog->items[i].tag == ITEM_FUNCTION && strcmp(prog->items[i].as.fn_decl.name, "main") == 0) {
                has_main = true;
                break;
            }
        }
        if (has_main) {
            uint8_t func_reg = alloc_reg();
            uint16_t main_ki = add_constant(value_string("main"));
            emit_ABx(ROP_GETGLOBAL, func_reg, main_ki, 0);
            emit_ABC(ROP_CALL, func_reg, 0, 1, 0);
            free_reg(func_reg);
        }
    }

finish: {
    /* Final return */
    uint8_t ret_reg = alloc_reg();
    emit_ABC(ROP_LOADUNIT, ret_reg, 0, 0, 0);
    emit_return(ret_reg, 0);
    free_reg(ret_reg);
}

    RegChunk *result = top.chunk;
    rc_cleanup(&top);
    rc = NULL;
    return result;
}

RegChunk *reg_compile_repl(const Program *prog, char **error) { return reg_compile_internal(prog, error, false, true); }

RegChunk *reg_compile_module(const Program *prog, char **error) {
    RegChunk *result = reg_compile_internal(prog, error, false, false);
    /* Copy export metadata from Program to RegChunk */
    if (result && prog->has_exports) {
        result->has_exports = true;
        result->export_count = prog->export_count;
        result->export_names = malloc(prog->export_count * sizeof(char *));
        if (!result->export_names) return NULL;
        for (size_t i = 0; i < prog->export_count; i++) result->export_names[i] = strdup(prog->export_names[i]);
    }
    return result;
}

void reg_compiler_free_known_enums(void) { rc_free_known_enums(); }
