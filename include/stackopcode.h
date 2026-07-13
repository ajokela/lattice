#ifndef STACKOPCODE_H
#define STACKOPCODE_H

typedef enum {
    /* Stack manipulation */
    OP_CONSTANT, /* Push constant from chunk.constants[operand] */
    OP_NIL,      /* Push nil */
    OP_TRUE,     /* Push true */
    OP_FALSE,    /* Push false */
    OP_UNIT,     /* Push unit */
    OP_POP,      /* Discard top of stack */
    OP_DUP,      /* Duplicate top of stack */
    OP_SWAP,     /* Swap top two stack values */

    /* Arithmetic/logical */
    OP_ADD,
    OP_SUB,
    OP_MUL,
    OP_DIV,
    OP_MOD,
    OP_NEG,
    OP_NOT,

    /* Bitwise */
    OP_BIT_AND,
    OP_BIT_OR,
    OP_BIT_XOR,
    OP_BIT_NOT,
    OP_LSHIFT,
    OP_RSHIFT,

    /* Comparison */
    OP_EQ,
    OP_NEQ,
    OP_LT,
    OP_GT,
    OP_LTEQ,
    OP_GTEQ,

    /* String concatenation */
    OP_CONCAT,

    /* Variables */
    OP_GET_LOCAL,     /* Push local from stack slot [operand] */
    OP_SET_LOCAL,     /* Set local at stack slot [operand], keep value on stack */
    OP_GET_GLOBAL,    /* env_get(constants[operand]) -> push result */
    OP_SET_GLOBAL,    /* env_set(constants[operand], TOS) */
    OP_DEFINE_GLOBAL, /* env_define(constants[operand], TOS), pop TOS */
    OP_GET_UPVALUE,   /* Push upvalue[operand] */
    OP_SET_UPVALUE,   /* Set upvalue[operand] = TOS */
    OP_CLOSE_UPVALUE, /* Close upvalue on top of stack */

    /* Jumps */
    OP_JUMP,            /* Unconditional jump by signed offset (2-byte operand) */
    OP_JUMP_IF_FALSE,   /* Jump if TOS falsy (does NOT pop) */
    OP_JUMP_IF_TRUE,    /* Jump if TOS truthy (does NOT pop) */
    OP_JUMP_IF_NOT_NIL, /* Jump if TOS is not nil (does NOT pop) */
    OP_LOOP,            /* Jump backward for loops (2-byte operand) */

    /* Functions/closures */
    OP_CALL,    /* Call TOS with [operand] args */
    OP_CLOSURE, /* Create closure from function constant */
    OP_RETURN,  /* Return TOS (or unit if none) */

    /* Iterators */
    OP_ITER_INIT, /* Convert range/array to iterator (push iterator state) */
    OP_ITER_NEXT, /* Push next value or jump if exhausted (2-byte offset) */

    /* Data structures */
    OP_BUILD_ARRAY,   /* Pop [operand] values, build array */
    OP_ARRAY_FLATTEN, /* Flatten one level of nested arrays (for spread) */
    OP_BUILD_MAP,     /* Pop [operand]*2 values (key, val pairs), build map */
    OP_BUILD_TUPLE,   /* Pop [operand] values, build tuple */
    OP_BUILD_STRUCT,  /* operand1 = name constant, operand2 = field count */
    OP_BUILD_RANGE,   /* Pop end, start -> push Range */
    OP_BUILD_ENUM,    /* operand1 = enum_name const, operand2 = variant_name const, operand3 = payload count */
    OP_INDEX,         /* Pop index, object -> push object[index] */
    OP_SET_INDEX,     /* Pop value, index, object -> set object[index] = value */
    OP_GET_FIELD,     /* operand = field name constant. Pop object, push field. */
    OP_SET_FIELD,     /* operand = field name constant. Pop value, pop object, set field. */
    OP_INVOKE,        /* operand1 = method name constant, operand2 = arg count */
    OP_INVOKE_LOCAL,  /* operand1 = local slot, operand2 = method name constant, operand3 = arg count. Mutates local
                         directly. */
    OP_INVOKE_GLOBAL, /* operand1 = global name constant, operand2 = method name constant, operand3 = arg count. Writes
                         back to env. */
    OP_SET_INDEX_LOCAL, /* operand = local slot. Pop value, index. Mutates local[index] = value. Push value. */

    /* Exception handling */
    OP_PUSH_EXCEPTION_HANDLER, /* operand = 2-byte jump offset to catch block */
    OP_POP_EXCEPTION_HANDLER,
    OP_THROW,      /* Pop error value, unwind to nearest handler */
    OP_TRY_UNWRAP, /* ? operator: unwrap ok or return err */

    /* Defer */
    OP_DEFER_PUSH, /* scope depth + reserved u16; consumes cleanup closure */
    OP_DEFER_RUN,  /* Execute all defers for current scope */

    /* Phase system */
    OP_FREEZE,     /* Pop value, push frozen copy */
    OP_THAW,       /* Pop value, push thawed copy */
    OP_CLONE,      /* Pop value, push deep clone */
    OP_MARK_FLUID, /* Set TOS phase to VTAG_FLUID (for flux bindings) */

    /* Phase system: reactions, bonds, seeds */
    OP_REACT,         /* operand: name_idx. Stack: [callback] → [unit] */
    OP_UNREACT,       /* operand: name_idx. Stack: [] → [unit] */
    OP_BOND,          /* operand: target_name_idx. Stack: [dep_str, strategy_str] → [unit] */
    OP_UNBOND,        /* operand: target_name_idx. Stack: [dep_str] → [unit] */
    OP_SEED,          /* operand: name_idx. Stack: [contract] → [unit] */
    OP_UNSEED,        /* operand: name_idx. Stack: [] → [unit] */
    OP_FREEZE_VAR,    /* operands: name_idx, loc_type, loc_slot. Stack: [value] → [frozen_clone] */
    OP_THAW_VAR,      /* operands: name_idx, loc_type, loc_slot. Stack: [value] → [thawed_clone] */
    OP_SUBLIMATE_VAR, /* operands: name_idx, loc_type, loc_slot. Stack: [value] → [sublimated_clone] */
    OP_SUBLIMATE,     /* no operands. Stack: [value] → [sublimated_value] */

    /* Builtins (fast path) */
    OP_PRINT, /* Pop [operand] values, print them */

    /* Module */
    OP_IMPORT, /* operand = path constant */

    /* Concurrency (variable-length instructions) */
    OP_SCOPE,  /* spawn_count(1), sync_idx(1), spawn_idx(1) × spawn_count */
    OP_SELECT, /* arm_count(1), per arm: flags(1), chan_idx(1), body_idx(1), binding_idx(1) */

    /* Specialized integer ops (fast path, no type checking overhead) */
    OP_INC_LOCAL, /* operand: slot. Increment int local by 1 in-place */
    OP_DEC_LOCAL, /* operand: slot. Decrement int local by 1 in-place */
    OP_ADD_INT,   /* Pop two ints, push int result */
    OP_SUB_INT,   /* Pop two ints, push int result */
    OP_MUL_INT,   /* Pop two ints, push int result */
    OP_LT_INT,    /* Pop two ints, push bool result */
    OP_LTEQ_INT,  /* Pop two ints, push bool result */
    OP_LOAD_INT8, /* operand: int8. Push small int from instruction stream */

    /* Wide constant index variants (2-byte BE index for >256 constants) */
    OP_CONSTANT_16,      /* Push constant from chunk.constants[BE16 operand] */
    OP_GET_GLOBAL_16,    /* env_get(constants[BE16 operand]) -> push result */
    OP_SET_GLOBAL_16,    /* env_set(constants[BE16 operand], TOS) */
    OP_DEFINE_GLOBAL_16, /* env_define(constants[BE16 operand], TOS), pop TOS */
    OP_CLOSURE_16,       /* Create closure from function constant (BE16 index) */
    OP_INVOKE_LOCAL_16,  /* Like INVOKE_LOCAL but method name constant is BE16 index */
    OP_INVOKE_GLOBAL_16, /* Like INVOKE_GLOBAL but name and method constant are BE16 indices */

    /* Ephemeral arena */
    OP_RESET_EPHEMERAL, /* Reset ephemeral bump arena at statement boundaries */

    /* Combined ops */
    OP_SET_LOCAL_POP, /* Set local at stack slot [operand] and pop (SET_LOCAL + POP) */

    /* Runtime type checking */
    OP_CHECK_TYPE,        /* operands: slot, type_name_const, err_msg_const. Check local matches type. */
    OP_CHECK_RETURN_TYPE, /* operands: type_name_const, err_msg_const. Check TOS matches type. */

    /* Phase queries */
    OP_IS_CRYSTAL,    /* Pop value, push bool (true if phase == VTAG_CRYSTAL) */
    OP_IS_FLUID,      /* Pop value, push bool (true if phase == VTAG_FLUID) */
    OP_FREEZE_EXCEPT, /* operands: name_idx, loc_type, loc_slot, except_count. Stack: [except_name1, ...] →
                         [frozen_value] */
    OP_FREEZE_FIELD,  /* operands: parent_name_idx, loc_type, loc_slot. Stack: [key_str] → [frozen_value] */

    /* String append (fast path for local += string) */
    OP_APPEND_STR_LOCAL, /* operands: slot. Pop TOS string, append to local[slot] in-place via realloc */

    /* Slice assignment */
    OP_SET_SLICE,       /* Pop end, start, obj, val -> splice obj[start..end] = val, push modified obj */
    OP_SET_SLICE_LOCAL, /* operand = local slot. Pop end, start, val -> splice local[start..end] = val */
    OP_INDEX_LOCAL,     /* operand = local slot. Pop index. Push local[index] element. */
    OP_GET_FIELD_LOCAL, /* operand1 = local slot, operand2 = field name constant. Push local.field. */

    /* Fallback to tree-walker */
    OP_HALT, /* Stop execution */

    /* Global index fast paths (borrow the global in env — no container clone).
     * Appended after OP_HALT so existing opcode values stay stable for the
     * self-hosted compiler, which hardcodes opcode numbers. */
    OP_INDEX_GLOBAL,     /* operand = BE16 name constant. Pop index. Push global[index] element. */
    OP_SET_INDEX_GLOBAL, /* operand = BE16 name constant. Pop index, value. Mutate global[index] = value in place. */

    /* Nested-element mutation (LAT-544). Emitted only for `obj[i].mutator()` where
     * the object is an EXPR_INDEX and the method name passes the mutator pre-filter.
     * Layout: [method_idx:u8][arg_count:u8][skip_len:u16 BE]. The index write-back
     * chain (skip_len bytes) is emitted immediately after this instruction. At
     * runtime, builtin_method_mutates() decides: if true, run the in-place builtin
     * on the cloned receiver, leave [result, mutated_clone], and fall through into
     * the write-back chain (which stores the clone back, error-checking the parent
     * phase). If false, advance past the chain (frame->ip += skip_len) and resolve
     * exactly like OP_INVOKE (struct/enum/map closure field, impl Type::method,
     * non-mutating builtin, or missing-method error). Kept after the legacy
     * opcode range so existing opcode numbers stay stable for the self-hosted
     * compiler. */
    OP_INVOKE_MUT,

    /* Inline select chooser (LAT-598). Operands were evaluated in source order
     * before this instruction. Layout: [arm_count:u8][flags:u8 x arm_count].
     * Consumes one Channel/timeout Int/default Unit per arm and pushes
     * [selected_index, received_value]. Appended for bytecode compatibility. */
    OP_SELECT_CHOOSE,

    /* Recursive match helpers. Appended for bytecode compatibility. */
    OP_HAS_FIELD,              /* operand = field-name constant. Pop value, push whether Struct/Map contains field. */
    OP_MATCH_RANGE,            /* Pop end/start, preserve candidate below them, push integer inclusive-range match. */
    OP_MATCH_FLUID,            /* Pop value, push phase == FLUID || phase == UNPHASED. */
    OP_CLOSE_UPVALUE_PRESERVE, /* Close local below TOS, remove local, preserve TOS without swapping slots. */
    OP_MATCH_TYPE,             /* operand = ValueType. Preserve candidate on TOS and push exact runtime-tag match. */

    /* Upvalue-receiver method call (LAT-621). Mirrors OP_INVOKE_LOCAL, but the
     * receiver is read through the frame's upvalue cell (ObjUpvalue->location:
     * open ⇒ enclosing frame's stack slot, closed ⇒ the cell's own storage), so
     * in-place builtin mutations (push/set/...) persist through the capture
     * instead of being applied to a discarded OP_GET_UPVALUE copy. Layout:
     * [uv_slot:u8][method_idx:u8][argc:u8]. Appended after the legacy range so
     * existing serialized bytecode keeps its numbering. */
    OP_INVOKE_UPVALUE,
    OP_INVOKE_UPVALUE_16, /* Like INVOKE_UPVALUE but method name constant is BE16: [uv_slot:u8][method_idx:u16][argc:u8]
                           */
} Opcode;

const char *opcode_name(Opcode op);

#endif /* STACKOPCODE_H */
