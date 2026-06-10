#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "test_backend.h"

#ifdef _WIN32
#include <windows.h>
#endif

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

#define MAX_TESTS 2048
static TestEntry all_tests[MAX_TESTS];
static int test_count = 0;

/* Track current test for crash diagnostics */
static const char *current_test_name = NULL;

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
        case BACKEND_STACK_VM: return "stack-vm";
        case BACKEND_REG_VM: return "regvm";
    }
    return "unknown";
}

/* Detect sanitizer: GCC uses __SANITIZE_ADDRESS__, Clang uses __has_feature */
#if defined(__SANITIZE_ADDRESS__)
#define LATTICE_HAS_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define LATTICE_HAS_ASAN 1
#endif
#endif

#ifndef LATTICE_HAS_ASAN
static void crash_handler(int sig) {
    const char *name = "unknown";
    switch (sig) {
        case SIGSEGV: name = "SIGSEGV"; break;
        case SIGFPE: name = "SIGFPE"; break;
        case SIGABRT: name = "SIGABRT"; break;
#ifdef SIGBUS
        case SIGBUS: name = "SIGBUS"; break;
#endif
    }
    fprintf(stderr, "\n*** CRASH: signal %s (%d) during test: %s ***\n", name, sig,
            current_test_name ? current_test_name : "(none)");
    fflush(stderr);
    _exit(128 + sig);
}
#endif

#ifdef _WIN32
/* Windows delivers crashes as SEH exceptions, which the POSIX signal() handler
 * above does not catch — so a crashing test silently kills the runner with no
 * name. Report the current test and the exception code (0xC0000005 access
 * violation, 0xC00000FD stack overflow, ...) before the process dies. */
static LONG WINAPI win_crash_filter(EXCEPTION_POINTERS *info) {
    unsigned long code = (info && info->ExceptionRecord) ? (unsigned long)info->ExceptionRecord->ExceptionCode : 0;
    const char *kind = "exception";
    if (code == 0xC0000005UL) kind = "ACCESS_VIOLATION";
    else if (code == 0xC00000FDUL) kind = "STACK_OVERFLOW";
    else if (code == 0xC000008CUL) kind = "ARRAY_BOUNDS_EXCEEDED";
    else if (code == 0xC0000094UL) kind = "INT_DIVIDE_BY_ZERO";
    fprintf(stderr, "\n*** CRASH: SEH %s (0x%lx) during test: %s ***\n", kind, code,
            current_test_name ? current_test_name : "(none)");
    fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER; /* terminate */
}
#endif

int main(int argc, char *argv[]) {
#ifndef LATTICE_HAS_ASAN
    signal(SIGSEGV, crash_handler);
    signal(SIGFPE, crash_handler);
    signal(SIGABRT, crash_handler);
#ifdef SIGBUS
    signal(SIGBUS, crash_handler);
#endif
#endif

#ifdef _WIN32
    /* Reserve stack so the crash filter can still run after a stack overflow,
     * then catch SEH crashes the POSIX handler misses. */
    ULONG stack_guarantee = 32 * 1024;
    SetThreadStackGuarantee(&stack_guarantee);
    SetUnhandledExceptionFilter(win_crash_filter);
#endif

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
        current_test_name = all_tests[i].name;
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
