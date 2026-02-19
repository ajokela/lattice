#ifndef OPCODE_H
#define OPCODE_H

typedef enum {
    /* Stack manipulation */
    OP_CONSTANT,       /* Push constant from chunk.constants[operand] */
    OP_NIL,            /* Push nil */
    OP_TRUE,           /* Push true */
    OP_FALSE,          /* Push false */
    OP_UNIT,           /* Push unit */
    OP_POP,            /* Discard top of stack */
    OP_DUP,            /* Duplicate top of stack */
    OP_SWAP,           /* Swap top two stack values */

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
    OP_GET_LOCAL,      /* Push local from stack slot [operand] */
    OP_SET_LOCAL,      /* Set local at stack slot [operand], keep value on stack */
    OP_GET_GLOBAL,     /* env_get(constants[operand]) -> push result */
    OP_SET_GLOBAL,     /* env_set(constants[operand], TOS) */
    OP_DEFINE_GLOBAL,  /* env_define(constants[operand], TOS), pop TOS */
    OP_GET_UPVALUE,    /* Push upvalue[operand] */
    OP_SET_UPVALUE,    /* Set upvalue[operand] = TOS */
    OP_CLOSE_UPVALUE,  /* Close upvalue on top of stack */

    /* Jumps */
    OP_JUMP,           /* Unconditional jump by signed offset (2-byte operand) */
    OP_JUMP_IF_FALSE,  /* Jump if TOS falsy (does NOT pop) */
    OP_JUMP_IF_TRUE,   /* Jump if TOS truthy (does NOT pop) */
    OP_JUMP_IF_NOT_NIL,/* Jump if TOS is not nil (does NOT pop) */
    OP_LOOP,           /* Jump backward for loops (2-byte operand) */

    /* Functions/closures */
    OP_CALL,           /* Call TOS with [operand] args */
    OP_CLOSURE,        /* Create closure from function constant */
    OP_RETURN,         /* Return TOS (or unit if none) */

    /* Iterators */
    OP_ITER_INIT,      /* Convert range/array to iterator (push iterator state) */
    OP_ITER_NEXT,      /* Push next value or jump if exhausted (2-byte offset) */

    /* Data structures */
    OP_BUILD_ARRAY,    /* Pop [operand] values, build array */
    OP_ARRAY_FLATTEN,  /* Flatten one level of nested arrays (for spread) */
    OP_BUILD_MAP,      /* Pop [operand]*2 values (key, val pairs), build map */
    OP_BUILD_TUPLE,    /* Pop [operand] values, build tuple */
    OP_BUILD_STRUCT,   /* operand1 = name constant, operand2 = field count */
    OP_BUILD_RANGE,    /* Pop end, start -> push Range */
    OP_BUILD_ENUM,     /* operand1 = enum_name const, operand2 = variant_name const, operand3 = payload count */
    OP_INDEX,          /* Pop index, object -> push object[index] */
    OP_SET_INDEX,      /* Pop value, index, object -> set object[index] = value */
    OP_GET_FIELD,      /* operand = field name constant. Pop object, push field. */
    OP_SET_FIELD,      /* operand = field name constant. Pop value, pop object, set field. */
    OP_INVOKE,         /* operand1 = method name constant, operand2 = arg count */
    OP_INVOKE_LOCAL,   /* operand1 = local slot, operand2 = method name constant, operand3 = arg count. Mutates local directly. */
    OP_INVOKE_GLOBAL,  /* operand1 = global name constant, operand2 = method name constant, operand3 = arg count. Writes back to env. */
    OP_SET_INDEX_LOCAL,/* operand = local slot. Pop value, index. Mutates local[index] = value. Push value. */

    /* Exception handling */
    OP_PUSH_EXCEPTION_HANDLER,  /* operand = 2-byte jump offset to catch block */
    OP_POP_EXCEPTION_HANDLER,
    OP_THROW,                   /* Pop error value, unwind to nearest handler */
    OP_TRY_UNWRAP,              /* ? operator: unwrap ok or return err */

    /* Defer */
    OP_DEFER_PUSH,     /* operand = 2-byte offset to defer body */
    OP_DEFER_RUN,      /* Execute all defers for current scope */

    /* Phase system */
    OP_FREEZE,         /* Pop value, push frozen copy */
    OP_THAW,           /* Pop value, push thawed copy */
    OP_CLONE,          /* Pop value, push deep clone */
    OP_MARK_FLUID,     /* Set TOS phase to VTAG_FLUID (for flux bindings) */

    /* Phase system: reactions, bonds, seeds */
    OP_REACT,          /* operand: name_idx. Stack: [callback] → [unit] */
    OP_UNREACT,        /* operand: name_idx. Stack: [] → [unit] */
    OP_BOND,           /* operand: target_name_idx. Stack: [dep_str, strategy_str] → [unit] */
    OP_UNBOND,         /* operand: target_name_idx. Stack: [dep_str] → [unit] */
    OP_SEED,           /* operand: name_idx. Stack: [contract] → [unit] */
    OP_UNSEED,         /* operand: name_idx. Stack: [] → [unit] */
    OP_FREEZE_VAR,     /* operands: name_idx, loc_type, loc_slot. Stack: [value] → [frozen_clone] */
    OP_THAW_VAR,       /* operands: name_idx, loc_type, loc_slot. Stack: [value] → [thawed_clone] */
    OP_SUBLIMATE_VAR,  /* operands: name_idx, loc_type, loc_slot. Stack: [value] → [sublimated_clone] */
    OP_SUBLIMATE,      /* no operands. Stack: [value] → [sublimated_value] */

    /* Builtins (fast path) */
    OP_PRINT,          /* Pop [operand] values, print them */

    /* Module */
    OP_IMPORT,         /* operand = path constant */

    /* Concurrency (variable-length instructions) */
    OP_SCOPE,          /* spawn_count(1), sync_idx(1), spawn_idx(1) × spawn_count */
    OP_SELECT,         /* arm_count(1), per arm: flags(1), chan_idx(1), body_idx(1), binding_idx(1) */

    /* Specialized integer ops (fast path, no type checking overhead) */
    OP_INC_LOCAL,      /* operand: slot. Increment int local by 1 in-place */
    OP_DEC_LOCAL,      /* operand: slot. Decrement int local by 1 in-place */
    OP_ADD_INT,        /* Pop two ints, push int result */
    OP_SUB_INT,        /* Pop two ints, push int result */
    OP_MUL_INT,        /* Pop two ints, push int result */
    OP_LT_INT,         /* Pop two ints, push bool result */
    OP_LTEQ_INT,       /* Pop two ints, push bool result */
    OP_LOAD_INT8,      /* operand: int8. Push small int from instruction stream */

    /* Wide constant index variants (2-byte BE index for >256 constants) */
    OP_CONSTANT_16,    /* Push constant from chunk.constants[BE16 operand] */
    OP_GET_GLOBAL_16,  /* env_get(constants[BE16 operand]) -> push result */
    OP_SET_GLOBAL_16,  /* env_set(constants[BE16 operand], TOS) */
    OP_DEFINE_GLOBAL_16, /* env_define(constants[BE16 operand], TOS), pop TOS */
    OP_CLOSURE_16,     /* Create closure from function constant (BE16 index) */

    /* Fallback to tree-walker */
    OP_HALT,           /* Stop execution */
} Opcode;

const char *opcode_name(Opcode op);

#endif /* OPCODE_H */
