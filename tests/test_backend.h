#ifndef TEST_BACKEND_H
#define TEST_BACKEND_H

typedef enum {
    BACKEND_TREE_WALK,  /* Evaluator (legacy) */
    BACKEND_STACK_VM,   /* Bytecode stack VM (production default) */
    BACKEND_REG_VM,     /* Register VM (POC) */
} TestBackend;

extern TestBackend test_backend;

#endif /* TEST_BACKEND_H */
