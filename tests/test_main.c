#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Minimal test harness */
static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

int test_current_failed = 0;

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

int main(void) {
    register_stdlib_tests();

    printf("Running %d tests...\n\n", test_count);

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
