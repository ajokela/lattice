#include "regopcode.h"

static const char *names[] = {
    [ROP_MOVE]         = "MOVE",
    [ROP_LOADK]        = "LOADK",
    [ROP_LOADI]        = "LOADI",
    [ROP_LOADNIL]      = "LOADNIL",
    [ROP_LOADTRUE]     = "LOADTRUE",
    [ROP_LOADFALSE]    = "LOADFALSE",
    [ROP_LOADUNIT]     = "LOADUNIT",
    [ROP_ADD]          = "ADD",
    [ROP_SUB]          = "SUB",
    [ROP_MUL]          = "MUL",
    [ROP_DIV]          = "DIV",
    [ROP_MOD]          = "MOD",
    [ROP_NEG]          = "NEG",
    [ROP_ADDI]         = "ADDI",
    [ROP_CONCAT]       = "CONCAT",
    [ROP_EQ]           = "EQ",
    [ROP_NEQ]          = "NEQ",
    [ROP_LT]           = "LT",
    [ROP_LTEQ]         = "LTEQ",
    [ROP_GT]           = "GT",
    [ROP_GTEQ]         = "GTEQ",
    [ROP_NOT]          = "NOT",
    [ROP_JMP]          = "JMP",
    [ROP_JMPFALSE]     = "JMPFALSE",
    [ROP_JMPTRUE]      = "JMPTRUE",
    [ROP_GETGLOBAL]    = "GETGLOBAL",
    [ROP_SETGLOBAL]    = "SETGLOBAL",
    [ROP_DEFINEGLOBAL] = "DEFINEGLOBAL",
    [ROP_GETFIELD]     = "GETFIELD",
    [ROP_SETFIELD]     = "SETFIELD",
    [ROP_GETINDEX]     = "GETINDEX",
    [ROP_SETINDEX]     = "SETINDEX",
    [ROP_GETUPVALUE]   = "GETUPVALUE",
    [ROP_SETUPVALUE]   = "SETUPVALUE",
    [ROP_CLOSEUPVALUE] = "CLOSEUPVALUE",
    [ROP_CALL]         = "CALL",
    [ROP_RETURN]       = "RETURN",
    [ROP_CLOSURE]      = "CLOSURE",
    [ROP_NEWARRAY]     = "NEWARRAY",
    [ROP_NEWSTRUCT]    = "NEWSTRUCT",
    [ROP_BUILDRANGE]   = "BUILDRANGE",
    [ROP_LEN]          = "LEN",
    [ROP_PRINT]        = "PRINT",
    [ROP_INVOKE]       = "INVOKE",
    [ROP_FREEZE]       = "FREEZE",
    [ROP_THAW]         = "THAW",
    [ROP_CLONE]        = "CLONE",
    [ROP_ITERINIT]     = "ITERINIT",
    [ROP_ITERNEXT]     = "ITERNEXT",
    [ROP_MARKFLUID]    = "MARKFLUID",
    /* Bitwise */
    [ROP_BIT_AND]      = "BIT_AND",
    [ROP_BIT_OR]       = "BIT_OR",
    [ROP_BIT_XOR]      = "BIT_XOR",
    [ROP_BIT_NOT]      = "BIT_NOT",
    [ROP_LSHIFT]       = "LSHIFT",
    [ROP_RSHIFT]       = "RSHIFT",
    /* Tuple */
    [ROP_NEWTUPLE]     = "NEWTUPLE",
    /* Spread/Flatten */
    [ROP_ARRAY_FLATTEN] = "ARRAY_FLATTEN",
    /* Enum */
    [ROP_NEWENUM]      = "NEWENUM",
    /* Optional chaining */
    [ROP_JMPNOTNIL]    = "JMPNOTNIL",
    /* Exception handling */
    [ROP_PUSH_HANDLER] = "PUSH_HANDLER",
    [ROP_POP_HANDLER]  = "POP_HANDLER",
    [ROP_THROW]        = "THROW",
    [ROP_TRY_UNWRAP]   = "TRY_UNWRAP",
    /* Defer */
    [ROP_DEFER_PUSH]   = "DEFER_PUSH",
    [ROP_DEFER_RUN]    = "DEFER_RUN",
    /* Variadic */
    [ROP_COLLECT_VARARGS] = "COLLECT_VARARGS",
    /* Advanced phase */
    [ROP_FREEZE_VAR]   = "FREEZE_VAR",
    [ROP_THAW_VAR]     = "THAW_VAR",
    [ROP_SUBLIMATE_VAR] = "SUBLIMATE_VAR",
    [ROP_REACT]        = "REACT",
    [ROP_UNREACT]      = "UNREACT",
    [ROP_BOND]         = "BOND",
    [ROP_UNBOND]       = "UNBOND",
    [ROP_SEED]         = "SEED",
    [ROP_UNSEED]       = "UNSEED",
    /* Module/Import */
    [ROP_IMPORT]       = "IMPORT",
    /* Concurrency */
    [ROP_SCOPE]        = "SCOPE",
    [ROP_SELECT]       = "SELECT",
    /* Ephemeral arena */
    [ROP_RESET_EPHEMERAL] = "RESET_EPHEMERAL",
    /* Optimization */
    [ROP_ADD_INT]      = "ADD_INT",
    [ROP_SUB_INT]      = "SUB_INT",
    [ROP_MUL_INT]      = "MUL_INT",
    [ROP_LT_INT]       = "LT_INT",
    [ROP_LTEQ_INT]     = "LTEQ_INT",
    [ROP_INC_REG]      = "INC_REG",
    [ROP_DEC_REG]      = "DEC_REG",
    [ROP_SETINDEX_LOCAL] = "SETINDEX_LOCAL",
    [ROP_INVOKE_GLOBAL] = "INVOKE_GLOBAL",
    /* Phase query */
    [ROP_IS_CRYSTAL]   = "IS_CRYSTAL",
    /* Type checking */
    [ROP_CHECK_TYPE]   = "CHECK_TYPE",
    [ROP_FREEZE_FIELD] = "FREEZE_FIELD",
    [ROP_THAW_FIELD]   = "THAW_FIELD",
    /* Require */
    [ROP_REQUIRE]      = "REQUIRE",
    /* Misc */
    [ROP_HALT]         = "HALT",
};

const char *reg_opcode_name(RegOpcode op) {
    if (op >= ROP_COUNT) return "UNKNOWN";
    return names[op] ? names[op] : "UNKNOWN";
}
