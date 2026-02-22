#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "test_backend.h"

/* Minimal test harness */
static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

int test_current_failed = 0;

/* Global backend selection (default: stack VM to match production) */
TestBackend test_backend = BACKEND_STACK_VM;

typedef void (*TestFn)(void);
typedef struct {
    const char *name;
    TestFn fn;
} TestEntry;

#define MAX_TESTS 1024
static TestEntry all_tests[MAX_TESTS];
static int test_count = 0;

void register_test(const char *name, TestFn fn) {
    if (test_count < MAX_TESTS) {
        all_tests[test_count].name = name;
        all_tests[test_count].fn = fn;
        test_count++;
    }
}

/* Forward declarations for manual registration */
extern void register_stdlib_tests(void);

static const char *backend_name(TestBackend b) {
    switch (b) {
        case BACKEND_TREE_WALK: return "tree-walk";
        case BACKEND_STACK_VM:  return "stack-vm";
        case BACKEND_REG_VM:   return "regvm";
    }
    return "unknown";
}

int main(int argc, char *argv[]) {
    /* Parse --backend flag */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--backend") == 0 && i + 1 < argc) {
            i++;
            if (strcmp(argv[i], "tree-walk") == 0) {
                test_backend = BACKEND_TREE_WALK;
            } else if (strcmp(argv[i], "stack-vm") == 0) {
                test_backend = BACKEND_STACK_VM;
            } else if (strcmp(argv[i], "regvm") == 0) {
                test_backend = BACKEND_REG_VM;
            } else {
                fprintf(stderr, "Unknown backend: %s\n", argv[i]);
                fprintf(stderr, "Valid backends: tree-walk, stack-vm, regvm\n");
                return 1;
            }
        }
    }

    register_stdlib_tests();

    printf("Running %d tests (backend: %s)...\n\n", test_count, backend_name(test_backend));

    for (int i = 0; i < test_count; i++) {
        test_current_failed = 0;
        tests_run++;
        all_tests[i].fn();
        if (test_current_failed) {
            tests_failed++;
            fprintf(stderr, "  FAILED: %s\n", all_tests[i].name);
        } else {
            tests_passed++;
            printf("  ok: %s\n", all_tests[i].name);
        }
    }

    printf("\nResults: %d passed, %d failed, %d total\n", tests_passed, tests_failed, tests_run);
    return tests_failed > 0 ? 1 : 0;
}
