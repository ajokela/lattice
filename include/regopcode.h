#ifndef REGOPCODE_H
#define REGOPCODE_H

#include <stdint.h>

/* ── Register-based instruction encoding ──
 *
 * 32-bit fixed-width instructions:
 *   [opcode: 8] [A: 8] [B: 8] [C: 8]     — ABC format
 *   [opcode: 8] [A: 8] [Bx: 16]           — ABx format (unsigned)
 *   [opcode: 8] [A: 8] [sBx: 16]          — AsBx format (signed)
 *   [opcode: 8] [sBx: 24]                 — sBx-only format (jumps)
 */

typedef uint32_t RegInstr;

/* Instruction encoding */
#define REG_ENCODE_ABC(op, a, b, c) \
    ((uint32_t)(op) | ((uint32_t)(a) << 8) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 24))

#define REG_ENCODE_ABx(op, a, bx) ((uint32_t)(op) | ((uint32_t)(a) << 8) | ((uint32_t)(bx) << 16))

#define REG_ENCODE_AsBx(op, a, sbx) \
    ((uint32_t)(op) | ((uint32_t)(a) << 8) | ((uint32_t)((uint16_t)(int16_t)(sbx)) << 16))

#define REG_ENCODE_sBx(op, sbx) ((uint32_t)(op) | ((uint32_t)((sbx) + 0x7FFFFF) << 8))

/* Instruction decoding */
#define REG_GET_OP(instr)    ((uint8_t)((instr) & 0xFF))
#define REG_GET_A(instr)     ((uint8_t)(((instr) >> 8) & 0xFF))
#define REG_GET_B(instr)     ((uint8_t)(((instr) >> 16) & 0xFF))
#define REG_GET_C(instr)     ((uint8_t)(((instr) >> 24) & 0xFF))
#define REG_GET_Bx(instr)    ((uint16_t)(((instr) >> 16) & 0xFFFF))
#define REG_GET_sBx(instr)   ((int16_t)(uint16_t)(((instr) >> 16) & 0xFFFF))
#define REG_GET_sBx24(instr) ((int32_t)(((instr) >> 8) & 0xFFFFFF) - 0x7FFFFF)

/* Register-based opcodes */
typedef enum {
    /* Data movement */
    ROP_MOVE,      /* A, B      : R[A] = R[B]                       */
    ROP_LOADK,     /* A, Bx     : R[A] = K[Bx]                     */
    ROP_LOADI,     /* A, sBx    : R[A] = int(sBx)                  */
    ROP_LOADNIL,   /* A         : R[A] = nil                        */
    ROP_LOADTRUE,  /* A         : R[A] = true                       */
    ROP_LOADFALSE, /* A         : R[A] = false                      */
    ROP_LOADUNIT,  /* A         : R[A] = unit                       */

    /* Arithmetic (3-address) */
    ROP_ADD,  /* A, B, C   : R[A] = R[B] + R[C]               */
    ROP_SUB,  /* A, B, C   : R[A] = R[B] - R[C]               */
    ROP_MUL,  /* A, B, C   : R[A] = R[B] * R[C]               */
    ROP_DIV,  /* A, B, C   : R[A] = R[B] / R[C]               */
    ROP_MOD,  /* A, B, C   : R[A] = R[B] % R[C]               */
    ROP_NEG,  /* A, B      : R[A] = -R[B]                     */
    ROP_ADDI, /* A, B, C   : R[A] = R[B] + signext(C)         */

    /* String concat */
    ROP_CONCAT, /* A, B, C   : R[A] = str(R[B]) .. str(R[C])    */

    /* Comparison */
    ROP_EQ,   /* A, B, C   : R[A] = (R[B] == R[C])            */
    ROP_NEQ,  /* A, B, C   : R[A] = (R[B] != R[C])            */
    ROP_LT,   /* A, B, C   : R[A] = (R[B] < R[C])             */
    ROP_LTEQ, /* A, B, C   : R[A] = (R[B] <= R[C])            */
    ROP_GT,   /* A, B, C   : R[A] = (R[B] > R[C])             */
    ROP_GTEQ, /* A, B, C   : R[A] = (R[B] >= R[C])            */
    ROP_NOT,  /* A, B      : R[A] = !R[B]                     */

    /* Control flow */
    ROP_JMP,      /* sBx(24)   : ip += sBx                        */
    ROP_JMPFALSE, /* A, sBx    : if !R[A]: ip += sBx              */
    ROP_JMPTRUE,  /* A, sBx    : if R[A]: ip += sBx               */

    /* Variables & Fields */
    ROP_GETGLOBAL,    /* A, Bx     : R[A] = globals[K[Bx]]            */
    ROP_SETGLOBAL,    /* A, Bx     : globals[K[Bx]] = R[A]            */
    ROP_DEFINEGLOBAL, /* A, Bx     : define globals[K[Bx]] = R[A]     */
    ROP_GETFIELD,     /* A, B, C   : R[A] = R[B].field[K[C]]          */
    ROP_SETFIELD,     /* A, B, C   : R[A].field[K[B]] = R[C]          */
    ROP_GETINDEX,     /* A, B, C   : R[A] = R[B][R[C]]                */
    ROP_SETINDEX,     /* A, B, C   : R[A][R[B]] = R[C]                */

    /* Upvalues */
    ROP_GETUPVALUE,   /* A, B      : R[A] = Upvalue[B]                */
    ROP_SETUPVALUE,   /* A, B      : Upvalue[B] = R[A]                */
    ROP_CLOSEUPVALUE, /* A         : close upvalue at R[A]             */

    /* Functions */
    ROP_CALL,    /* A, B, C   : R[A..A+C-1] = R[A](R[A+1..A+B]) */
    ROP_RETURN,  /* A, B      : return R[A..A+B-1]               */
    ROP_CLOSURE, /* A, Bx     : R[A] = closure(K[Bx])            */

    /* Data structures */
    ROP_NEWARRAY,   /* A, B, C   : R[A] = [R[B..B+C-1]]            */
    ROP_NEWSTRUCT,  /* A, B, C   : R[A] = struct K[B] with C fields */
    ROP_BUILDRANGE, /* A, B, C   : R[A] = range(R[B], R[C])         */
    ROP_LEN,        /* A, B      : R[A] = len(R[B])                 */

    /* Builtins */
    ROP_PRINT,  /* A, B      : print(R[A..A+B-1])               */
    ROP_INVOKE, /* A, B, C   : R[A] = R[A].method(K[B])(args)   */
    ROP_FREEZE, /* A, B      : R[A] = freeze(R[B])              */
    ROP_THAW,   /* A, B      : R[A] = thaw(R[B])                */
    ROP_CLONE,  /* A, B      : R[A] = clone(R[B])               */

    /* Iterator */
    ROP_ITERINIT, /* A, B      : R[A] = iter(R[B]), R[A+1] = idx  */
    ROP_ITERNEXT, /* A, B, sBx : R[A] = next(R[B]); done → jmp   */

    /* Phase */
    ROP_MARKFLUID, /* A         : R[A].phase = FLUID               */

    /* Bitwise operations */
    ROP_BIT_AND, /* A, B, C   : R[A] = R[B] & R[C]               */
    ROP_BIT_OR,  /* A, B, C   : R[A] = R[B] | R[C]               */
    ROP_BIT_XOR, /* A, B, C   : R[A] = R[B] ^ R[C]               */
    ROP_BIT_NOT, /* A, B      : R[A] = ~R[B]                     */
    ROP_LSHIFT,  /* A, B, C   : R[A] = R[B] << R[C]              */
    ROP_RSHIFT,  /* A, B, C   : R[A] = R[B] >> R[C]              */

    /* Tuple */
    ROP_NEWTUPLE, /* A, B, C   : R[A] = tuple(R[B..B+C-1])        */

    /* Spread/Flatten */
    ROP_ARRAY_FLATTEN, /* A, B      : R[A] = flatten(R[B])              */

    /* Enum with payload */
    ROP_NEWENUM, /* A, B, C   : R[A] = enum(base=B, argc=C)      */

    /* Optional chaining */
    ROP_JMPNOTNIL, /* A, sBx    : if R[A] != nil, ip += sBx        */

    /* Exception handling */
    ROP_PUSH_HANDLER, /* A, sBx    : push handler, catch at ip+sBx    */
    ROP_POP_HANDLER,  /*           : pop handler on success            */
    ROP_THROW,        /* A         : throw R[A]                        */
    ROP_TRY_UNWRAP,   /* A         : unwrap ok or propagate err        */

    /* Defer */
    ROP_DEFER_PUSH, /* sBx       : register defer body, skip past    */
    ROP_DEFER_RUN,  /*           : execute defers in LIFO order      */

    /* Destructuring (compiler-only, uses existing ops) */
    /* No new opcodes needed */

    /* Variadic */
    ROP_COLLECT_VARARGS, /* A, B    : R[A] = array of excess args >= B  */

    /* Advanced phase system */
    ROP_FREEZE_VAR,    /* A, B, C   : freeze var K[A], loc B:C          */
    ROP_THAW_VAR,      /* A, B, C   : thaw var K[A], loc B:C            */
    ROP_SUBLIMATE_VAR, /* A, B, C   : sublimate var K[A], loc B:C       */
    ROP_REACT,         /* A, B      : register reaction R[A] on R[B]    */
    ROP_UNREACT,       /* A         : remove reaction R[A]              */
    ROP_BOND,          /* A, B      : bond R[A] depends on R[B]         */
    ROP_UNBOND,        /* A         : remove bond R[A]                  */
    ROP_SEED,          /* A, B      : register seed contract R[A] on B  */
    ROP_UNSEED,        /* A         : remove seed R[A]                  */

    /* Module/Import */
    ROP_IMPORT, /* A, Bx     : R[A] = import(K[Bx])             */

    /* Concurrency */
    ROP_SCOPE,  /* variable-length: spawn_count, sync/spawn idx  */
    ROP_SELECT, /* variable-length: arm_count, per-arm data      */

    /* Ephemeral arena */
    ROP_RESET_EPHEMERAL, /*         : reset bump arena                  */

    /* Optimization opcodes */
    ROP_ADD_INT,        /* A, B, C   : R[A] = R[B].int + R[C].int (no check) */
    ROP_SUB_INT,        /* A, B, C   : R[A] = R[B].int - R[C].int (no check) */
    ROP_MUL_INT,        /* A, B, C   : R[A] = R[B].int * R[C].int (no check) */
    ROP_LT_INT,         /* A, B, C   : R[A] = R[B].int < R[C].int (no check) */
    ROP_LTEQ_INT,       /* A, B, C   : R[A] = R[B].int <= R[C].int (no check)*/
    ROP_INC_REG,        /* A         : R[A]++ (assumes int)              */
    ROP_DEC_REG,        /* A         : R[A]-- (assumes int)              */
    ROP_SETINDEX_LOCAL, /* A, B, C   : R[A][R[B]] = R[C] (in-place)     */
    ROP_INVOKE_GLOBAL,  /* 2-word: [INVOKE_GLOBAL dst, name_ki, argc] [method_ki, args_base, 0] */
    ROP_INVOKE_LOCAL,   /* 2-word: [INVOKE_LOCAL dst, local_reg, argc] [method_ki, args_base, 0] */

    /* Phase query */
    ROP_IS_CRYSTAL, /* A, B      : R[A] = (R[B].phase == CRYSTAL)    */
    ROP_IS_FLUID,   /* A, B      : R[A] = (R[B].phase == FLUID)      */

    /* Type checking */
    ROP_CHECK_TYPE, /* A, Bx     : throw if type_name(R[A]) != K[Bx] + err word */

    /* Per-field phase control */
    ROP_FREEZE_FIELD,  /* A, B, C   : freeze R[A].field[K[B]] (mark CRYSTAL)      */
    ROP_THAW_FIELD,    /* A, B, C   : mark R[A].field[K[B]] as FLUID (key_phases) */
    ROP_FREEZE_EXCEPT, /* 2-word: [FREEZE_EXCEPT A=name_ki, B=loc_type, C=slot] [except_base, except_count, 0] */

    /* Require (no scope isolation, defs go to global) */
    ROP_REQUIRE, /* A, Bx     : require(K[Bx]), R[A] = bool      */

    /* Slice assignment */
    ROP_SETSLICE,       /* A, B, C   : R[A][R[B]..R[B+1]] = R[C] (splice) */
    ROP_SETSLICE_LOCAL, /* A, B, C   : R[A][R[B]..R[B+1]] = R[C] (in-place)*/

    /* Misc */
    ROP_HALT, /* stop execution                                */

    ROP_COUNT /* number of opcodes                             */
} RegOpcode;

const char *reg_opcode_name(RegOpcode op);

#endif /* REGOPCODE_H */
