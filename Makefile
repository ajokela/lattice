CC      = cc
CFLAGS  = -std=c11 -Wall -Wextra -Werror -Iinclude -O3 -DVM_USE_COMPUTED_GOTO
LDFLAGS =

SRC_DIR    = src
BUILD_DIR  = build
TEST_DIR   = tests

# ── Windows cross-compilation (MinGW) ──
ifdef WINDOWS
    CC       = x86_64-w64-mingw32-gcc
    CFLAGS   = -std=c11 -Wall -Wextra -Werror -Wno-implicit-fallthrough -Iinclude -O3 -D_WIN32_WINNT=0x0600
    # Windows defaults to a 1 MB main-thread stack (vs 8 MB on Linux/macOS), which
    # the recursive-descent parser's depth limit (1000) overflows before it can
    # reject — making the LAT-417/413 stack-overflow DoS guards ineffective and
    # crashing on deeply nested input. Reserve 16 MB so the limits behave as on
    # other platforms (reserve is address space, committed on demand).
    LDFLAGS  = -lws2_32 -lpthread -lsecur32 -lcrypt32 -ladvapi32 -static -Wl,--stack,0x1000000
    TARGET   = clat.exe
    RELEASE_NAME = clat-windows-x86_64.exe
    # Skip editline and computed goto on Windows; use Schannel for TLS
    EDIT_CFLAGS  =
    EDIT_LDFLAGS =
    TLS_CFLAGS   = -DLATTICE_HAS_TLS -DLATTICE_TLS_SCHANNEL
    TLS_LDFLAGS  =
    SKIP_AUTODETECT = 1
endif

# UNAME_S is needed for release naming even with WINDOWS=1
UNAME_S := $(shell uname -s)

ifndef SKIP_AUTODETECT
# ── Optional libedit / readline ──
EDIT_AVAILABLE := $(shell pkg-config --exists libedit 2>/dev/null && echo yes || echo no)
ifeq ($(EDIT_AVAILABLE),yes)
    EDIT_CFLAGS  := $(shell pkg-config --cflags libedit) -DLATTICE_HAS_EDITLINE
    ifdef STATIC
        EDIT_LDFLAGS := $(shell pkg-config --libs --static libedit)
    else
        EDIT_LDFLAGS := $(shell pkg-config --libs libedit)
    endif
else
    # Try linking -ledit directly (macOS ships it without pkg-config on some setups)
    EDIT_TEST := $(shell echo 'int main(){return 0;}' | $(CC) -x c - -ledit -o /dev/null 2>/dev/null && echo yes || echo no)
    ifeq ($(EDIT_TEST),yes)
        EDIT_CFLAGS  := -DLATTICE_HAS_EDITLINE
        EDIT_LDFLAGS := -ledit
    else
        # Try libreadline as a fallback
        EDIT_TEST_RL := $(shell echo 'int main(){return 0;}' | $(CC) -x c - -lreadline -o /dev/null 2>/dev/null && echo yes || echo no)
        ifeq ($(EDIT_TEST_RL),yes)
            EDIT_CFLAGS  := -DLATTICE_HAS_READLINE
            EDIT_LDFLAGS := -lreadline
        else
            EDIT_CFLAGS  :=
            EDIT_LDFLAGS :=
        endif
    endif
endif
CFLAGS  += $(EDIT_CFLAGS)
LDFLAGS += $(EDIT_LDFLAGS)

# ── Optional TLS (OpenSSL) ──
TLS_AVAILABLE := $(shell pkg-config --exists openssl 2>/dev/null && echo yes || echo no)
ifeq ($(TLS_AVAILABLE),yes)
    TLS_CFLAGS  := $(shell pkg-config --cflags openssl) -DLATTICE_HAS_TLS
    ifdef STATIC
        TLS_LDFLAGS := $(shell pkg-config --libs --static openssl)
    else
        TLS_LDFLAGS := $(shell pkg-config --libs openssl)
    endif
endif
ifeq ($(TLS),0)
    TLS_CFLAGS  :=
    TLS_LDFLAGS :=
endif
ifneq ($(TLS),0)
ifeq ($(TLS_AVAILABLE),no)
    TLS_DIRECT_TEST := $(shell echo 'int main(){return 0;}' | \
        $(CC) -x c - -lssl -lcrypto -o /dev/null 2>/dev/null && echo yes || echo no)
    ifeq ($(TLS_DIRECT_TEST),yes)
        TLS_CFLAGS  := -DLATTICE_HAS_TLS
        TLS_LDFLAGS := -lssl -lcrypto
    endif
endif
endif
# ── pthreads and dlopen (Linux needs explicit linking) ──
ifeq ($(UNAME_S),Linux)
    CFLAGS  += -D_GNU_SOURCE
    LDFLAGS += -lpthread -lm -ldl -rdynamic
endif
ifeq ($(UNAME_S),FreeBSD)
    LDFLAGS += -lpthread -lm
endif
ifeq ($(UNAME_S),OpenBSD)
    LDFLAGS += -lpthread -lm
endif
ifeq ($(UNAME_S),NetBSD)
    CFLAGS  += -I/usr/pkg/include
    LDFLAGS += -L/usr/pkg/lib -lpthread -lm
endif

# Static linking (use STATIC=1 for fully static builds, e.g. in Alpine Docker)
ifdef STATIC
    LDFLAGS += -static
endif
endif # SKIP_AUTODETECT

# TLS flags must apply on every platform — the Windows block above sets them
# explicitly (Schannel) while skipping autodetection, so this append has to
# live outside the SKIP_AUTODETECT guard
CFLAGS  += $(TLS_CFLAGS)
LDFLAGS += $(TLS_LDFLAGS)

# Source files
SRCS = $(SRC_DIR)/main.c \
       $(SRC_DIR)/ds/str.c \
       $(SRC_DIR)/ds/vec.c \
       $(SRC_DIR)/ds/hashmap.c \
       $(SRC_DIR)/token.c \
       $(SRC_DIR)/lexer.c \
       $(SRC_DIR)/ast.c \
       $(SRC_DIR)/parser.c \
       $(SRC_DIR)/value.c \
       $(SRC_DIR)/env.c \
       $(SRC_DIR)/eval.c \
       $(SRC_DIR)/memory.c \
       $(SRC_DIR)/phase_check.c \
       $(SRC_DIR)/string_ops.c \
       $(SRC_DIR)/builtins.c \
       $(SRC_DIR)/net.c \
       $(SRC_DIR)/tls.c \
       $(SRC_DIR)/json.c \
       $(SRC_DIR)/math_ops.c \
       $(SRC_DIR)/env_ops.c \
       $(SRC_DIR)/time_ops.c \
       $(SRC_DIR)/fs_ops.c \
       $(SRC_DIR)/process_ops.c \
       $(SRC_DIR)/type_ops.c \
       $(SRC_DIR)/datetime_ops.c \
       $(SRC_DIR)/regex_ops.c \
       $(SRC_DIR)/format_ops.c \
       $(SRC_DIR)/path_ops.c \
       $(SRC_DIR)/crypto_ops.c \
       $(SRC_DIR)/array_ops.c \
       $(SRC_DIR)/channel.c \
       $(SRC_DIR)/http.c \
       $(SRC_DIR)/toml_ops.c \
       $(SRC_DIR)/yaml_ops.c \
       $(SRC_DIR)/ext.c \
       $(SRC_DIR)/stackopcode.c \
       $(SRC_DIR)/chunk.c \
       $(SRC_DIR)/stackcompiler.c \
       $(SRC_DIR)/stackvm.c \
       $(SRC_DIR)/runtime.c \
       $(SRC_DIR)/intern.c \
       $(SRC_DIR)/latc.c \
       $(SRC_DIR)/regopcode.c \
       $(SRC_DIR)/regcompiler.c \
       $(SRC_DIR)/regvm.c \
       $(SRC_DIR)/builtin_methods.c \
       $(SRC_DIR)/match_check.c \
       $(SRC_DIR)/package.c \
       $(SRC_DIR)/formatter.c \
       $(SRC_DIR)/debugger.c \
       $(SRC_DIR)/dap.c \
       $(SRC_DIR)/completion.c \
       $(SRC_DIR)/doc_gen.c \
       $(SRC_DIR)/iterator.c \
       $(SRC_DIR)/gc.c \
       $(SRC_DIR)/progress.c \
       vendor/cJSON.c

# All source files except main.c (for tests)
LIB_SRCS = $(filter-out $(SRC_DIR)/main.c, $(SRCS))

OBJS     = $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(filter $(SRC_DIR)/%,$(SRCS))) \
           $(patsubst vendor/%.c,$(BUILD_DIR)/vendor/%.o,$(filter vendor/%,$(SRCS)))
LIB_OBJS = $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(filter $(SRC_DIR)/%,$(LIB_SRCS))) \
           $(patsubst vendor/%.c,$(BUILD_DIR)/vendor/%.o,$(filter vendor/%,$(LIB_SRCS)))

ifndef WINDOWS
TARGET = clat
endif

# Thin bytecode runtime (clat-run): VMs + stdlib + bytecode loader + stubs.
# Executes both .latc (stack VM) and .rlat (register VM) bytecode.
# Excludes: lexer, parser, ast, eval, compilers, lsp, formatter,
#           debugger, completion, doc_gen, package, phase_check, match_check.
RUNTIME_SRCS = $(SRC_DIR)/runtime_main.c \
               $(SRC_DIR)/runtime_stubs.c \
               $(SRC_DIR)/ds/str.c \
               $(SRC_DIR)/ds/vec.c \
               $(SRC_DIR)/ds/hashmap.c \
               $(SRC_DIR)/value.c \
               $(SRC_DIR)/env.c \
               $(SRC_DIR)/memory.c \
               $(SRC_DIR)/string_ops.c \
               $(SRC_DIR)/builtins.c \
               $(SRC_DIR)/net.c \
               $(SRC_DIR)/tls.c \
               $(SRC_DIR)/json.c \
               $(SRC_DIR)/math_ops.c \
               $(SRC_DIR)/env_ops.c \
               $(SRC_DIR)/time_ops.c \
               $(SRC_DIR)/fs_ops.c \
               $(SRC_DIR)/process_ops.c \
               $(SRC_DIR)/type_ops.c \
               $(SRC_DIR)/datetime_ops.c \
               $(SRC_DIR)/regex_ops.c \
               $(SRC_DIR)/format_ops.c \
               $(SRC_DIR)/path_ops.c \
               $(SRC_DIR)/crypto_ops.c \
               $(SRC_DIR)/array_ops.c \
               $(SRC_DIR)/channel.c \
               $(SRC_DIR)/http.c \
               $(SRC_DIR)/toml_ops.c \
               $(SRC_DIR)/yaml_ops.c \
               $(SRC_DIR)/ext.c \
               $(SRC_DIR)/stackopcode.c \
               $(SRC_DIR)/chunk.c \
               $(SRC_DIR)/stackvm.c \
               $(SRC_DIR)/regopcode.c \
               $(SRC_DIR)/regvm.c \
               $(SRC_DIR)/runtime.c \
               $(SRC_DIR)/intern.c \
               $(SRC_DIR)/latc.c \
               $(SRC_DIR)/builtin_methods.c \
               $(SRC_DIR)/iterator.c \
               $(SRC_DIR)/gc.c \
               $(SRC_DIR)/progress.c

RUNTIME_OBJS = $(RUNTIME_SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)
ifdef WINDOWS
RUNTIME_TARGET = clat-run.exe
else
RUNTIME_TARGET = clat-run
endif

# LSP server sources
LSP_SRCS = $(SRC_DIR)/lsp_main.c \
           $(SRC_DIR)/lsp_server.c \
           $(SRC_DIR)/lsp_protocol.c \
           $(SRC_DIR)/lsp_analysis.c \
           $(SRC_DIR)/lsp_symbols.c \
           vendor/cJSON.c

LSP_SRC_OBJS = $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(filter $(SRC_DIR)/%,$(LSP_SRCS)))
LSP_VND_OBJS = $(patsubst vendor/%.c,$(BUILD_DIR)/vendor/%.o,$(filter vendor/%,$(LSP_SRCS)))
LSP_OBJS = $(LSP_SRC_OBJS) $(LSP_VND_OBJS)
LSP_TARGET = clat-lsp

# LSP library objects (without lsp_main.c, for linking into tests)
LSP_LIB_SRCS = $(SRC_DIR)/lsp_server.c \
               $(SRC_DIR)/lsp_protocol.c \
               $(SRC_DIR)/lsp_analysis.c \
               $(SRC_DIR)/lsp_symbols.c \
               vendor/cJSON.c
LSP_LIB_SRC_OBJS = $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(filter $(SRC_DIR)/%,$(LSP_LIB_SRCS)))
LSP_LIB_VND_OBJS = $(patsubst vendor/%.c,$(BUILD_DIR)/vendor/%.o,$(filter vendor/%,$(LSP_LIB_SRCS)))
LSP_LIB_OBJS = $(LSP_LIB_SRC_OBJS) $(LSP_LIB_VND_OBJS)

# Test sources
TEST_SRCS = $(TEST_DIR)/test_main.c \
            $(TEST_DIR)/test_ds.c \
            $(TEST_DIR)/test_memory.c \
            $(TEST_DIR)/test_eval.c \
            $(TEST_DIR)/test_stdlib.c \
            $(TEST_DIR)/test_lsp.c \
            $(TEST_DIR)/test_lsp_integration.c \
            $(TEST_DIR)/test_latc.c \
            $(TEST_DIR)/test_debugger.c \
            $(TEST_DIR)/test_doc_gen.c \
            $(TEST_DIR)/test_formatter.c \
            $(TEST_DIR)/test_transfer.c \
            $(TEST_DIR)/test_stats_cli.c

TEST_OBJS   = $(TEST_SRCS:$(TEST_DIR)/%.c=$(BUILD_DIR)/tests/%.o)
TEST_TARGET = $(BUILD_DIR)/test_runner
ifdef WINDOWS
PROCESS_FIXTURE = $(BUILD_DIR)/process_fixture.exe
else
PROCESS_FIXTURE = $(BUILD_DIR)/process_fixture
endif

# WASM build — exclude tree-walk evaluator and CLI-only modules to reduce binary size
WASM_EXCLUDE = $(SRC_DIR)/eval.c $(SRC_DIR)/phase_check.c $(SRC_DIR)/match_check.c \
               $(SRC_DIR)/completion.c $(SRC_DIR)/doc_gen.c $(SRC_DIR)/formatter.c \
               $(SRC_DIR)/dap.c vendor/cJSON.c
WASM_SRCS   = $(filter-out $(WASM_EXCLUDE), $(LIB_SRCS)) $(SRC_DIR)/wasm_api.c
WASM_OUT    = lattice-lang.org/lattice.js
WASM_FLAGS  = -std=gnu11 -Wall -Wextra -Wno-error -Wno-constant-conversion -Iinclude -O2 \
              -sEXPORTED_FUNCTIONS=_lat_init,_lat_run_line,_lat_is_complete,_lat_destroy,_lat_init_regvm,_lat_run_line_regvm,_lat_destroy_regvm,_lat_get_error,_lat_clear_error,_lat_heap_bytes,_free \
              -sEXPORTED_RUNTIME_METHODS=ccall,cwrap,UTF8ToString \
              -sMODULARIZE=1 -sEXPORT_NAME=createLattice \
              -sALLOW_MEMORY_GROWTH=1

# Fuzz harnesses
FUZZ_DIR    = fuzz
FUZZ_SRC    = $(FUZZ_DIR)/fuzz_eval.c
FUZZ_OBJ    = $(BUILD_DIR)/fuzz/fuzz_eval.o
FUZZ_TARGET = $(BUILD_DIR)/fuzz_eval

# Bytecode deserializer fuzz harness
FUZZ_LATC_SRC    = $(FUZZ_DIR)/fuzz_latc.c
FUZZ_LATC_OBJ    = $(BUILD_DIR)/fuzz/fuzz_latc.o
FUZZ_LATC_TARGET = $(BUILD_DIR)/fuzz_latc

# VM/RegVM fuzz harness (combined)
FUZZ_VM_SRC    = $(FUZZ_DIR)/fuzz_vm.c
FUZZ_VM_OBJ    = $(BUILD_DIR)/fuzz/fuzz_vm.o
FUZZ_VM_TARGET = $(BUILD_DIR)/fuzz_vm

# Stack VM fuzz harness (standalone)
FUZZ_STACKVM_SRC    = $(FUZZ_DIR)/fuzz_stackvm.c
FUZZ_STACKVM_OBJ    = $(BUILD_DIR)/fuzz/fuzz_stackvm.o
FUZZ_STACKVM_TARGET = $(BUILD_DIR)/fuzz_stackvm

# RegVM fuzz harness (standalone)
FUZZ_REGVM_SRC    = $(FUZZ_DIR)/fuzz_regvm.c
FUZZ_REGVM_OBJ    = $(BUILD_DIR)/fuzz/fuzz_regvm.o
FUZZ_REGVM_TARGET = $(BUILD_DIR)/fuzz_regvm

# JSON parser fuzz harness
FUZZ_JSON_SRC    = $(FUZZ_DIR)/fuzz_json.c
FUZZ_JSON_OBJ    = $(BUILD_DIR)/fuzz/fuzz_json.o
FUZZ_JSON_TARGET = $(BUILD_DIR)/fuzz_json

# TOML parser fuzz harness
FUZZ_TOML_SRC    = $(FUZZ_DIR)/fuzz_toml.c
FUZZ_TOML_OBJ    = $(BUILD_DIR)/fuzz/fuzz_toml.o
FUZZ_TOML_TARGET = $(BUILD_DIR)/fuzz_toml

# YAML parser fuzz harness
FUZZ_YAML_SRC    = $(FUZZ_DIR)/fuzz_yaml.c
FUZZ_YAML_OBJ    = $(BUILD_DIR)/fuzz/fuzz_yaml.o
FUZZ_YAML_TARGET = $(BUILD_DIR)/fuzz_yaml

# Lexer-only fuzz harness
FUZZ_LEXER_SRC    = $(FUZZ_DIR)/fuzz_lexer.c
FUZZ_LEXER_OBJ    = $(BUILD_DIR)/fuzz/fuzz_lexer.o
FUZZ_LEXER_TARGET = $(BUILD_DIR)/fuzz_lexer

# Formatter fuzz harness
FUZZ_FORMATTER_SRC    = $(FUZZ_DIR)/fuzz_formatter.c
FUZZ_FORMATTER_OBJ    = $(BUILD_DIR)/fuzz/fuzz_formatter.o
FUZZ_FORMATTER_TARGET = $(BUILD_DIR)/fuzz_formatter

# FS ops fuzz harness (Windows-focused: read-only path functions)
FUZZ_FS_SRC    = $(FUZZ_DIR)/fuzz_fs.c
FUZZ_FS_OBJ    = $(BUILD_DIR)/fuzz/fuzz_fs.o
FUZZ_FS_TARGET = $(BUILD_DIR)/fuzz_fs

# Cross-compiled Windows FS fuzzer: MinGW + ASan + SanitizerCoverage + minifuzz.
# libFuzzer is unsupported for the mingw target, so fuzz/minifuzz.c supplies a
# coverage-guided driver. Build from Linux with llvm-mingw's clang:
#   make fuzz-fs-win WINFUZZ_CC=/opt/llvm-mingw/bin/x86_64-w64-mingw32-clang
FUZZ_MINIFUZZ_OBJ  = $(BUILD_DIR)/fuzz/minifuzz.o
FUZZ_FS_WIN_TARGET = $(BUILD_DIR)/fuzz_fs.exe
WINFUZZ_CC ?= clang

# Parser fuzz harness
FUZZ_PARSER_SRC    = $(FUZZ_DIR)/fuzz_parser.c
FUZZ_PARSER_OBJ    = $(BUILD_DIR)/fuzz/fuzz_parser.o
FUZZ_PARSER_TARGET = $(BUILD_DIR)/fuzz_parser

.PHONY: all clean test test-tree-walk test-regvm test-all-backends test-ballistics-lab test-force-copy test-latc test-runtime test-bootstrap asan asan-all tsan coverage analyze clang-tidy fuzz fuzz-latc fuzz-vm fuzz-stackvm fuzz-regvm fuzz-json fuzz-toml fuzz-yaml fuzz-lexer fuzz-formatter fuzz-parser fuzz-fs fuzz-fs-win fuzz-all fuzz-seed wasm bench bench-regvm bench-stress bench-all bench-freeze-gate bench-cbr ext-pg ext-sqlite ext-ffi ext-redis ext-websocket ext-image lsp runtime runtime-release deploy-coverage install uninstall release

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

# LSP server
lsp: $(LSP_TARGET)

$(LSP_TARGET): $(LIB_OBJS) $(LSP_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Thin bytecode runtime
runtime: $(RUNTIME_TARGET)

$(RUNTIME_TARGET): $(RUNTIME_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(BUILD_DIR)/vendor/%.o: vendor/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -Wno-unused-parameter -Wno-deprecated-declarations -c -o $@ $<

$(BUILD_DIR)/tests/%.o: $(TEST_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

$(PROCESS_FIXTURE): $(TEST_DIR)/process_fixture.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $<

# Auto-generated header dependencies: without these, header edits (e.g. a
# LATC_FORMAT bump) leave stale objects behind and incremental builds lie
-include $(wildcard $(BUILD_DIR)/*.d $(BUILD_DIR)/ds/*.d $(BUILD_DIR)/vendor/*.d $(BUILD_DIR)/tests/*.d $(BUILD_DIR)/fuzz/*.d)

$(TEST_TARGET): $(LIB_OBJS) $(LSP_LIB_OBJS) $(TEST_OBJS) | $(PROCESS_FIXTURE)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# LAT-520: require_ext() no longer searches the CWD-relative ./extensions/ by
# default (library-planting hardening). The in-tree test extensions live in
# ./extensions/<name>/; the test runner executes from the trusted repo root, so
# it opts in explicitly via LATTICE_EXT_ALLOW_CWD=1.
test: $(TEST_TARGET) $(LSP_TARGET)
	LATTICE_EXT_ALLOW_CWD=1 ./$(BUILD_DIR)/test_runner

test-tree-walk: $(TEST_TARGET) $(LSP_TARGET)
	LATTICE_EXT_ALLOW_CWD=1 ./$(BUILD_DIR)/test_runner --backend tree-walk

test-regvm: $(TEST_TARGET) $(LSP_TARGET)
	LATTICE_EXT_ALLOW_CWD=1 ./$(BUILD_DIR)/test_runner --backend regvm

BALLISTICS_LAB_TESTS = test_units.lat test_domain.lat test_reference_import.lat reference_experiment.lat

test-ballistics-lab: $(TARGET)
	@for backend in "" "--tree-walk" "--regvm"; do \
		label="$$backend"; test -n "$$label" || label="--stack-vm"; \
		echo "=== ballistics_lab backend $$label ==="; \
		for test_file in $(BALLISTICS_LAB_TESTS); do \
			./$(TARGET) $$backend examples/ballistics_lab/$$test_file || exit 1; \
		done; \
	done

test-all-backends: $(TEST_TARGET) $(LSP_TARGET) test-ballistics-lab
	@echo "=== stack-vm ===" && LATTICE_EXT_ALLOW_CWD=1 ./$(BUILD_DIR)/test_runner --backend stack-vm
	@echo "=== tree-walk ===" && LATTICE_EXT_ALLOW_CWD=1 ./$(BUILD_DIR)/test_runner --backend tree-walk
	@echo "=== regvm ===" && LATTICE_EXT_ALLOW_CWD=1 ./$(BUILD_DIR)/test_runner --backend regvm

# LAT-449 Round B differential oracle: LATTICE_FORCE_COPY=1 disables the
# Crystal-by-Reference borrow fast path in value_clone_impl (every clone is a
# physical deep copy) while freeze still materializes shared regions and
# value_free still releases them — rc traffic without aliasing. Program
# semantics must be bit-identical to a normal run; any divergence is an
# aliasing bug (a retained handle treated as a private copy, or a missed
# value_unshare). A handful of test_memory.c tests that assert the aliasing
# MECHANISM itself (handle identity, retain counts) self-skip under the
# oracle — see cbr_force_copy_active().
test-force-copy: $(TEST_TARGET) $(LSP_TARGET)
	@echo "=== FORCE_COPY oracle: stack-vm ===" && LATTICE_EXT_ALLOW_CWD=1 LATTICE_FORCE_COPY=1 ./$(BUILD_DIR)/test_runner --backend stack-vm
	@echo "=== FORCE_COPY oracle: tree-walk ===" && LATTICE_EXT_ALLOW_CWD=1 LATTICE_FORCE_COPY=1 ./$(BUILD_DIR)/test_runner --backend tree-walk
	@echo "=== FORCE_COPY oracle: regvm ===" && LATTICE_EXT_ALLOW_CWD=1 LATTICE_FORCE_COPY=1 ./$(BUILD_DIR)/test_runner --backend regvm

LATC_TESTS = latc_roundtrip latc_structs latc_match latc_new_features latc_traits latc_traits_codegen \
             latc_expressions latc_variables latc_control_flow latc_functions \
             latc_data_structures latc_enums_match latc_structs_impl latc_error_handling \
             latc_string_interp latc_spread_test latc_tuple_test \
             latc_impl_test latc_impl_advanced latc_import_test \
             latc_match_enum latc_match_test latc_try_test \
             latc_from_import_test latc_scope_test \
             latc_defer_test latc_select_test latc_type_check latc_variadic_defaults latc_optimized_ops \
             latc_test_blocks latc_nested_index latc_slice

test-latc: $(TARGET)
	@PASS=0; FAIL=0; \
	for name in $(LATC_TESTS); do \
		printf "%-30s" "$$name..."; \
		LATC_OUT=$$(./$(TARGET) compiler/latc.lat tests/$$name.lat /tmp/$$name.latc 2>&1); \
		if [ $$? -ne 0 ]; then \
			echo "FAIL (compile)"; \
			echo "  $$LATC_OUT"; \
			FAIL=$$((FAIL + 1)); \
			continue; \
		fi; \
		./$(TARGET) /tmp/$$name.latc > /tmp/$$name.compiled.out 2>/dev/null; \
		if [ $$? -ne 0 ]; then \
			echo "FAIL (run .latc)"; \
			cat /tmp/$$name.compiled.out; \
			FAIL=$$((FAIL + 1)); \
			rm -f /tmp/$$name.latc /tmp/$$name.compiled.out; \
			continue; \
		fi; \
		./$(TARGET) tests/$$name.lat > /tmp/$$name.direct.out 2>/dev/null; \
		if [ $$? -ne 0 ]; then \
			echo "FAIL (run .lat)"; \
			cat /tmp/$$name.direct.out; \
			FAIL=$$((FAIL + 1)); \
			rm -f /tmp/$$name.latc /tmp/$$name.compiled.out /tmp/$$name.direct.out; \
			continue; \
		fi; \
		if diff /tmp/$$name.compiled.out /tmp/$$name.direct.out > /tmp/$$name.diff 2>&1; then \
			echo "PASS"; \
			PASS=$$((PASS + 1)); \
		else \
			echo "FAIL (output mismatch)"; \
			head -10 /tmp/$$name.diff; \
			FAIL=$$((FAIL + 1)); \
		fi; \
		rm -f /tmp/$$name.latc /tmp/$$name.compiled.out /tmp/$$name.direct.out /tmp/$$name.diff; \
	done; \
	echo ""; \
	echo "--- Deterministic serialization ---"; \
	DPASS=0; DFAIL=0; \
	for name in latc_expressions latc_variables latc_control_flow latc_functions; do \
		printf "%-30s" "deterministic:$$name..."; \
		./$(TARGET) compiler/latc.lat tests/$$name.lat /tmp/$$name.1.latc > /dev/null 2>&1; \
		./$(TARGET) compiler/latc.lat tests/$$name.lat /tmp/$$name.2.latc > /dev/null 2>&1; \
		if cmp -s /tmp/$$name.1.latc /tmp/$$name.2.latc; then \
			echo "PASS"; \
			DPASS=$$((DPASS + 1)); \
		else \
			echo "FAIL (non-deterministic)"; \
			DFAIL=$$((DFAIL + 1)); \
		fi; \
		rm -f /tmp/$$name.1.latc /tmp/$$name.2.latc; \
	done; \
	PASS=$$((PASS + DPASS)); \
	FAIL=$$((FAIL + DFAIL)); \
	echo ""; \
	echo "--- Compile-error rejection (LAT-444) ---"; \
	printf "%-30s" "reject:nested_slice..."; \
	printf 'flux g = [[1,2,3,4],[5,6,7,8]]\ng[0][1..3] = [90, 91]\n' > /tmp/latc_nested_slice.lat; \
	CE_OUT=$$(./$(TARGET) compiler/latc.lat /tmp/latc_nested_slice.lat /tmp/latc_nested_slice.latc 2>&1); \
	if [ $$? -ne 0 ] && echo "$$CE_OUT" | grep -q "slice assignment through a nested index chain is not supported"; then \
		echo "PASS"; \
		PASS=$$((PASS + 1)); \
	else \
		echo "FAIL (expected compile error, got: $$CE_OUT)"; \
		FAIL=$$((FAIL + 1)); \
	fi; \
	rm -f /tmp/latc_nested_slice.lat /tmp/latc_nested_slice.latc; \
	echo ""; \
	echo "Results: $$PASS passed, $$FAIL failed"; \
	if [ $$FAIL -gt 0 ]; then exit 1; fi

# Thin-runtime roundtrip tests: compile to .latc (stack VM) and .rlat
# (register VM), execute each with clat-run, and diff against direct
# execution of the source on the corresponding backend.
RUNTIME_TESTS = latc_expressions latc_variables latc_control_flow latc_functions \
                latc_data_structures latc_error_handling latc_string_interp \
                latc_cbr_freeze_alias

test-runtime: $(TARGET) $(RUNTIME_TARGET)
	@PASS=0; FAIL=0; \
	for name in $(RUNTIME_TESTS); do \
		printf "%-35s" "runtime-latc:$$name..."; \
		./$(TARGET) compile tests/$$name.lat -o /tmp/rt_$$name.latc > /dev/null 2>&1; \
		./$(RUNTIME_TARGET) /tmp/rt_$$name.latc > /tmp/rt_$$name.out 2>&1; \
		./$(TARGET) tests/$$name.lat > /tmp/rt_$$name.want 2>&1; \
		if diff /tmp/rt_$$name.out /tmp/rt_$$name.want > /dev/null 2>&1; then \
			echo "PASS"; PASS=$$((PASS + 1)); \
		else \
			echo "FAIL"; diff /tmp/rt_$$name.out /tmp/rt_$$name.want | head -5; FAIL=$$((FAIL + 1)); \
		fi; \
		rm -f /tmp/rt_$$name.latc /tmp/rt_$$name.out /tmp/rt_$$name.want; \
	done; \
	for name in $(RUNTIME_TESTS); do \
		printf "%-35s" "runtime-rlat:$$name..."; \
		./$(TARGET) compile --regvm tests/$$name.lat -o /tmp/rt_$$name.rlat > /dev/null 2>&1; \
		./$(RUNTIME_TARGET) /tmp/rt_$$name.rlat > /tmp/rt_$$name.out 2>&1; \
		./$(TARGET) --regvm tests/$$name.lat > /tmp/rt_$$name.want 2>&1; \
		if diff /tmp/rt_$$name.out /tmp/rt_$$name.want > /dev/null 2>&1; then \
			echo "PASS"; PASS=$$((PASS + 1)); \
		else \
			echo "FAIL"; diff /tmp/rt_$$name.out /tmp/rt_$$name.want | head -5; FAIL=$$((FAIL + 1)); \
		fi; \
		rm -f /tmp/rt_$$name.rlat /tmp/rt_$$name.out /tmp/rt_$$name.want; \
	done; \
	echo ""; \
	echo "Results: $$PASS passed, $$FAIL failed"; \
	if [ $$FAIL -gt 0 ]; then exit 1; fi

# Bootstrap fixed point: the self-hosted compiler compiles itself (stage1),
# stage1 compiles the compiler again (stage2), and stage1 == stage2 bit-for-bit.
test-bootstrap: $(TARGET)
	@printf "%-30s" "bootstrap:stage1..."; \
	OUT=$$(./$(TARGET) compiler/latc.lat compiler/latc.lat /tmp/latc_stage1.latc 2>&1); \
	if [ $$? -ne 0 ]; then \
		echo "FAIL (stage1 compile)"; \
		echo "  $$OUT"; \
		exit 1; \
	fi; \
	echo "PASS"; \
	printf "%-30s" "bootstrap:stage2..."; \
	OUT=$$(./$(TARGET) /tmp/latc_stage1.latc compiler/latc.lat /tmp/latc_stage2.latc 2>&1); \
	if [ $$? -ne 0 ]; then \
		echo "FAIL (stage2 compile)"; \
		echo "  $$OUT"; \
		rm -f /tmp/latc_stage1.latc; \
		exit 1; \
	fi; \
	echo "PASS"; \
	printf "%-30s" "bootstrap:fixed-point..."; \
	if cmp -s /tmp/latc_stage1.latc /tmp/latc_stage2.latc; then \
		echo "PASS"; \
		echo ""; \
		echo "Bootstrap fixed point reached: stage1 == stage2"; \
		rm -f /tmp/latc_stage1.latc /tmp/latc_stage2.latc; \
	else \
		echo "FAIL (stage1 != stage2)"; \
		rm -f /tmp/latc_stage1.latc /tmp/latc_stage2.latc; \
		exit 1; \
	fi

asan: CFLAGS += -fsanitize=address,undefined -g -O1
asan: LDFLAGS += -fsanitize=address,undefined
asan: clean $(TEST_TARGET) $(LSP_TARGET)
	./$(BUILD_DIR)/test_runner

asan-all: CFLAGS += -fsanitize=address,undefined -g -O1
asan-all: LDFLAGS += -fsanitize=address,undefined
asan-all: clean $(TEST_TARGET) $(LSP_TARGET)
	@echo "=== asan: stack-vm ===" && LATTICE_EXT_ALLOW_CWD=1 ./$(BUILD_DIR)/test_runner --backend stack-vm
	@echo "=== asan: tree-walk ===" && LATTICE_EXT_ALLOW_CWD=1 ./$(BUILD_DIR)/test_runner --backend tree-walk
	@echo "=== asan: regvm ===" ; LATTICE_EXT_ALLOW_CWD=1 ./$(BUILD_DIR)/test_runner --backend regvm; \
	 rc=$$?; if [ $$rc -gt 128 ]; then exit $$rc; fi

# Stage 5 (LAT-457): the TSan gate runs the full suite on all three
# backends plus the CbR alias and concurrency matrices through an
# instrumented clat. abort_on_error=0 makes a red run fail with TSan's
# exit code instead of SIGABRT-at-exit; suppressions (documented, scoped
# to non-CbR noise only) live in tests/tsan.supp. fork()-based tests
# self-skip under TSan (LAT_TSAN_BUILD in tests/).
TSAN_OPTS = abort_on_error=0 suppressions=$(CURDIR)/tests/tsan.supp
tsan: CFLAGS += -fsanitize=thread -g -O1
tsan: LDFLAGS += -fsanitize=thread
tsan: clean $(TEST_TARGET) $(LSP_TARGET) $(TARGET)
	@echo "=== tsan: tests (stack-vm) ===" && TSAN_OPTIONS="$(TSAN_OPTS)" ./$(BUILD_DIR)/test_runner --backend stack-vm
	@echo "=== tsan: tests (tree-walk) ===" && TSAN_OPTIONS="$(TSAN_OPTS)" ./$(BUILD_DIR)/test_runner --backend tree-walk
	@echo "=== tsan: tests (regvm) ===" && TSAN_OPTIONS="$(TSAN_OPTS)" ./$(BUILD_DIR)/test_runner --backend regvm
	@for b in "" "--tree-walk" "--regvm"; do \
		echo "=== tsan: cbr_alias_matrix $$b ==="; \
		TSAN_OPTIONS="$(TSAN_OPTS)" ./$(TARGET) $$b tests/cbr_alias_matrix.lat || exit 1; \
		echo "=== tsan: cbr_concurrency_matrix $$b ==="; \
		TSAN_OPTIONS="$(TSAN_OPTS)" ./$(TARGET) $$b tests/cbr_concurrency_matrix.lat || exit 1; \
	done; \
	echo "tsan: ALL GREEN"

coverage: CFLAGS += -fprofile-instr-generate -fcoverage-mapping -g -O0
coverage: LDFLAGS += -fprofile-instr-generate
coverage: clean $(TEST_TARGET) $(LSP_TARGET)
	LLVM_PROFILE_FILE=$(BUILD_DIR)/stackvm.profraw ./$(BUILD_DIR)/test_runner
	LLVM_PROFILE_FILE=$(BUILD_DIR)/treewalk.profraw ./$(BUILD_DIR)/test_runner --backend tree-walk
	LLVM_PROFILE_FILE=$(BUILD_DIR)/regvm.profraw ./$(BUILD_DIR)/test_runner --backend regvm
	llvm-profdata merge -sparse $(BUILD_DIR)/stackvm.profraw $(BUILD_DIR)/treewalk.profraw $(BUILD_DIR)/regvm.profraw -o $(BUILD_DIR)/default.profdata
	llvm-cov report ./$(BUILD_DIR)/test_runner -instr-profile=$(BUILD_DIR)/default.profdata $(LIB_SRCS)
	llvm-cov show ./$(BUILD_DIR)/test_runner -instr-profile=$(BUILD_DIR)/default.profdata \
		-format=html -output-dir=$(BUILD_DIR)/coverage $(LIB_SRCS)
	cp scripts/coverage-style.css $(BUILD_DIR)/coverage/style.css
	find $(BUILD_DIR)/coverage -name '*.html' -exec sed -i '' 's|</head>|<link href="https://fonts.googleapis.com/css2?family=Inter:wght@400;500;600\&display=swap" rel="stylesheet"></head>|' {} +
	@echo "\n==> Coverage report: $(BUILD_DIR)/coverage/index.html"

ifeq ($(UNAME_S),Darwin)
    ANALYZE_CC = xcrun clang
else
    ANALYZE_CC = clang
endif

SUPPRESSIONS = scripts/analyzer-suppressions.txt

analyze:
	@ALL_OUT=$$(mktemp); \
	for f in $(LIB_SRCS); do \
		$(ANALYZE_CC) --analyze -std=c11 -Iinclude $(EDIT_CFLAGS) $(TLS_CFLAGS) $$f 2>&1 || true; \
	done > "$$ALL_OUT" 2>&1; \
	TOTAL=$$(grep -c 'warning:' "$$ALL_OUT" || true); \
	if [ -f $(SUPPRESSIONS) ]; then \
		PATTERN=$$(grep -v '^#' $(SUPPRESSIONS) | grep -v '^$$' | paste -sd'|' -); \
		NEW_OUT=$$(mktemp); \
		grep 'warning:' "$$ALL_OUT" | grep -Ev "$$PATTERN" > "$$NEW_OUT" || true; \
		NEW_COUNT=$$(wc -l < "$$NEW_OUT" | tr -d ' '); \
	else \
		NEW_OUT=$$(mktemp); \
		grep 'warning:' "$$ALL_OUT" > "$$NEW_OUT" || true; \
		NEW_COUNT=$$(wc -l < "$$NEW_OUT" | tr -d ' '); \
	fi; \
	SUPPRESSED=$$((TOTAL - NEW_COUNT)); \
	if [ "$$NEW_COUNT" -gt 0 ]; then \
		cat "$$NEW_OUT"; \
		echo ""; \
		echo "==> $$TOTAL warnings total, $$SUPPRESSED suppressed, $$NEW_COUNT new warnings need attention"; \
	else \
		echo "==> $$TOTAL warnings total, all suppressed (known false positives)"; \
	fi; \
	rm -f "$$ALL_OUT" "$$NEW_OUT"

clang-tidy:
	@echo "==> Running clang-tidy on source files..."
	@FAIL=0; \
	for f in $(LIB_SRCS); do \
		clang-tidy $$f -- -std=c11 -Iinclude $(EDIT_CFLAGS) $(TLS_CFLAGS) -D_GNU_SOURCE 2>&1 || FAIL=1; \
	done; \
	if [ $$FAIL -eq 0 ]; then echo "==> clang-tidy: all clean"; \
	else echo "==> clang-tidy: issues found"; exit 1; fi

$(BUILD_DIR)/fuzz/%.o: $(FUZZ_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

# libFuzzer requires full LLVM clang (not Apple clang).
# Override FUZZ_CC to point to your LLVM installation if not at the default path.
FUZZ_CC ?= $(shell [ -x /opt/homebrew/opt/llvm/bin/clang ] && echo /opt/homebrew/opt/llvm/bin/clang || echo clang)
fuzz: CC = $(FUZZ_CC)
fuzz: CFLAGS = -std=c11 -D_GNU_SOURCE -Iinclude $(EDIT_CFLAGS) $(TLS_CFLAGS) -fsanitize=fuzzer,address,undefined -g -O1
fuzz: LDFLAGS = $(EDIT_LDFLAGS) $(TLS_LDFLAGS) -fsanitize=fuzzer,address,undefined
fuzz: clean $(LIB_OBJS) $(FUZZ_OBJ)
	$(CC) $(CFLAGS) -o $(FUZZ_TARGET) $(LIB_OBJS) $(FUZZ_OBJ) $(LDFLAGS)
	@mkdir -p fuzz/corpus
	@echo "\n==> Fuzzer built: $(FUZZ_TARGET)"
	@echo "    Run:  $(FUZZ_TARGET) fuzz/corpus/ -max_len=4096"
	@echo "    Seed: cp examples/*.lat benchmarks/*.lat fuzz/corpus/"

fuzz-latc: CC = $(FUZZ_CC)
fuzz-latc: CFLAGS = -std=c11 -D_GNU_SOURCE -Iinclude $(EDIT_CFLAGS) $(TLS_CFLAGS) -fsanitize=fuzzer,address,undefined -g -O1
fuzz-latc: LDFLAGS = $(EDIT_LDFLAGS) $(TLS_LDFLAGS) -fsanitize=fuzzer,address,undefined
fuzz-latc: clean $(LIB_OBJS) $(FUZZ_LATC_OBJ)
	$(CC) $(CFLAGS) -o $(FUZZ_LATC_TARGET) $(LIB_OBJS) $(FUZZ_LATC_OBJ) $(LDFLAGS)
	@mkdir -p fuzz/corpus_latc
	@echo "\n==> Bytecode fuzzer built: $(FUZZ_LATC_TARGET)"
	@echo "    Run:  $(FUZZ_LATC_TARGET) fuzz/corpus_latc/ -max_len=65536"

fuzz-vm: CC = $(FUZZ_CC)
fuzz-vm: CFLAGS = -std=c11 -D_GNU_SOURCE -Iinclude $(EDIT_CFLAGS) $(TLS_CFLAGS) -fsanitize=fuzzer,address,undefined -g -O1
fuzz-vm: LDFLAGS = $(EDIT_LDFLAGS) $(TLS_LDFLAGS) -fsanitize=fuzzer,address,undefined
fuzz-vm: clean $(LIB_OBJS) $(FUZZ_VM_OBJ)
	$(CC) $(CFLAGS) -o $(FUZZ_VM_TARGET) $(LIB_OBJS) $(FUZZ_VM_OBJ) $(LDFLAGS)
	@mkdir -p fuzz/corpus
	@echo "\n==> VM fuzzer built: $(FUZZ_VM_TARGET)"
	@echo "    Run:  $(FUZZ_VM_TARGET) fuzz/corpus/ -max_len=4096"
	@echo "    Seed: make fuzz-seed"

fuzz-stackvm: CC = $(FUZZ_CC)
fuzz-stackvm: CFLAGS = -std=c11 -D_GNU_SOURCE -Iinclude $(EDIT_CFLAGS) $(TLS_CFLAGS) -fsanitize=fuzzer,address,undefined -g -O1
fuzz-stackvm: LDFLAGS = $(EDIT_LDFLAGS) $(TLS_LDFLAGS) -fsanitize=fuzzer,address,undefined
fuzz-stackvm: clean $(LIB_OBJS) $(FUZZ_STACKVM_OBJ)
	$(CC) $(CFLAGS) -o $(FUZZ_STACKVM_TARGET) $(LIB_OBJS) $(FUZZ_STACKVM_OBJ) $(LDFLAGS)
	@mkdir -p fuzz/corpus
	@echo "\n==> Stack VM fuzzer built: $(FUZZ_STACKVM_TARGET)"
	@echo "    Run:  $(FUZZ_STACKVM_TARGET) fuzz/corpus/ -max_len=4096"
	@echo "    Seed: make fuzz-seed"

fuzz-regvm: CC = $(FUZZ_CC)
fuzz-regvm: CFLAGS = -std=c11 -D_GNU_SOURCE -Iinclude $(EDIT_CFLAGS) $(TLS_CFLAGS) -fsanitize=fuzzer,address,undefined -g -O1
fuzz-regvm: LDFLAGS = $(EDIT_LDFLAGS) $(TLS_LDFLAGS) -fsanitize=fuzzer,address,undefined
fuzz-regvm: clean $(LIB_OBJS) $(FUZZ_REGVM_OBJ)
	$(CC) $(CFLAGS) -o $(FUZZ_REGVM_TARGET) $(LIB_OBJS) $(FUZZ_REGVM_OBJ) $(LDFLAGS)
	@mkdir -p fuzz/corpus
	@echo "\n==> RegVM fuzzer built: $(FUZZ_REGVM_TARGET)"
	@echo "    Run:  $(FUZZ_REGVM_TARGET) fuzz/corpus/ -max_len=4096"
	@echo "    Seed: make fuzz-seed"

fuzz-fs: CC = $(FUZZ_CC)
fuzz-fs: CFLAGS = -std=c11 -D_GNU_SOURCE -Iinclude $(EDIT_CFLAGS) $(TLS_CFLAGS) -fsanitize=fuzzer,address,undefined -g -O1
fuzz-fs: LDFLAGS = $(EDIT_LDFLAGS) $(TLS_LDFLAGS) -fsanitize=fuzzer,address,undefined
fuzz-fs: clean $(LIB_OBJS) $(FUZZ_FS_OBJ)
	$(CC) $(CFLAGS) -o $(FUZZ_FS_TARGET) $(LIB_OBJS) $(FUZZ_FS_OBJ) $(LDFLAGS)
	@echo "\n==> FS fuzzer built: $(FUZZ_FS_TARGET)"

fuzz-fs-win: CC = $(WINFUZZ_CC)
fuzz-fs-win: CFLAGS = -std=c11 -Iinclude -D_WIN32_WINNT=0x0600 --target=x86_64-w64-windows-gnu -fsanitize=address -fsanitize-coverage=trace-pc-guard -g -O1
fuzz-fs-win: LDFLAGS = -fsanitize=address -lpthread -lws2_32 -lbcrypt -lcrypt32
fuzz-fs-win: clean $(LIB_OBJS) $(FUZZ_FS_OBJ) $(FUZZ_MINIFUZZ_OBJ)
	$(CC) $(CFLAGS) -o $(FUZZ_FS_WIN_TARGET) $(LIB_OBJS) $(FUZZ_FS_OBJ) $(FUZZ_MINIFUZZ_OBJ) $(LDFLAGS)
	@echo "\n==> Windows FS fuzzer built: $(FUZZ_FS_WIN_TARGET)"

fuzz-json: CC = $(FUZZ_CC)
fuzz-json: CFLAGS = -std=c11 -D_GNU_SOURCE -Iinclude $(EDIT_CFLAGS) $(TLS_CFLAGS) -fsanitize=fuzzer,address,undefined -g -O1
fuzz-json: LDFLAGS = $(EDIT_LDFLAGS) $(TLS_LDFLAGS) -fsanitize=fuzzer,address,undefined
fuzz-json: clean $(LIB_OBJS) $(FUZZ_JSON_OBJ)
	$(CC) $(CFLAGS) -o $(FUZZ_JSON_TARGET) $(LIB_OBJS) $(FUZZ_JSON_OBJ) $(LDFLAGS)
	@mkdir -p fuzz/corpus_json
	@echo "\n==> JSON fuzzer built: $(FUZZ_JSON_TARGET)"
	@echo "    Run:  $(FUZZ_JSON_TARGET) fuzz/corpus_json/ -max_len=4096"

fuzz-toml: CC = $(FUZZ_CC)
fuzz-toml: CFLAGS = -std=c11 -D_GNU_SOURCE -Iinclude $(EDIT_CFLAGS) $(TLS_CFLAGS) -fsanitize=fuzzer,address,undefined -g -O1
fuzz-toml: LDFLAGS = $(EDIT_LDFLAGS) $(TLS_LDFLAGS) -fsanitize=fuzzer,address,undefined
fuzz-toml: clean $(LIB_OBJS) $(FUZZ_TOML_OBJ)
	$(CC) $(CFLAGS) -o $(FUZZ_TOML_TARGET) $(LIB_OBJS) $(FUZZ_TOML_OBJ) $(LDFLAGS)
	@mkdir -p fuzz/corpus_toml
	@echo "\n==> TOML fuzzer built: $(FUZZ_TOML_TARGET)"
	@echo "    Run:  $(FUZZ_TOML_TARGET) fuzz/corpus_toml/ -max_len=4096"

fuzz-yaml: CC = $(FUZZ_CC)
fuzz-yaml: CFLAGS = -std=c11 -D_GNU_SOURCE -Iinclude $(EDIT_CFLAGS) $(TLS_CFLAGS) -fsanitize=fuzzer,address,undefined -g -O1
fuzz-yaml: LDFLAGS = $(EDIT_LDFLAGS) $(TLS_LDFLAGS) -fsanitize=fuzzer,address,undefined
fuzz-yaml: clean $(LIB_OBJS) $(FUZZ_YAML_OBJ)
	$(CC) $(CFLAGS) -o $(FUZZ_YAML_TARGET) $(LIB_OBJS) $(FUZZ_YAML_OBJ) $(LDFLAGS)
	@mkdir -p fuzz/corpus_yaml
	@echo "\n==> YAML fuzzer built: $(FUZZ_YAML_TARGET)"
	@echo "    Run:  $(FUZZ_YAML_TARGET) fuzz/corpus_yaml/ -max_len=4096"

fuzz-lexer: CC = $(FUZZ_CC)
fuzz-lexer: CFLAGS = -std=c11 -D_GNU_SOURCE -Iinclude $(EDIT_CFLAGS) $(TLS_CFLAGS) -fsanitize=fuzzer,address,undefined -g -O1
fuzz-lexer: LDFLAGS = $(EDIT_LDFLAGS) $(TLS_LDFLAGS) -fsanitize=fuzzer,address,undefined
fuzz-lexer: clean $(LIB_OBJS) $(FUZZ_LEXER_OBJ)
	$(CC) $(CFLAGS) -o $(FUZZ_LEXER_TARGET) $(LIB_OBJS) $(FUZZ_LEXER_OBJ) $(LDFLAGS)
	@mkdir -p fuzz/corpus
	@echo "\n==> Lexer fuzzer built: $(FUZZ_LEXER_TARGET)"
	@echo "    Run:  $(FUZZ_LEXER_TARGET) fuzz/corpus/ -max_len=4096"
	@echo "    Seed: make fuzz-seed"

fuzz-formatter: CC = $(FUZZ_CC)
fuzz-formatter: CFLAGS = -std=c11 -D_GNU_SOURCE -Iinclude $(EDIT_CFLAGS) $(TLS_CFLAGS) -fsanitize=fuzzer,address,undefined -g -O1
fuzz-formatter: LDFLAGS = $(EDIT_LDFLAGS) $(TLS_LDFLAGS) -fsanitize=fuzzer,address,undefined
fuzz-formatter: clean $(LIB_OBJS) $(FUZZ_FORMATTER_OBJ)
	$(CC) $(CFLAGS) -o $(FUZZ_FORMATTER_TARGET) $(LIB_OBJS) $(FUZZ_FORMATTER_OBJ) $(LDFLAGS)
	@mkdir -p fuzz/corpus
	@echo "\n==> Formatter fuzzer built: $(FUZZ_FORMATTER_TARGET)"
	@echo "    Run:  $(FUZZ_FORMATTER_TARGET) fuzz/corpus/ -max_len=4096"
	@echo "    Seed: make fuzz-seed"

fuzz-parser: CC = $(FUZZ_CC)
fuzz-parser: CFLAGS = -std=c11 -D_GNU_SOURCE -Iinclude $(EDIT_CFLAGS) $(TLS_CFLAGS) -fsanitize=fuzzer,address,undefined -g -O1
fuzz-parser: LDFLAGS = $(EDIT_LDFLAGS) $(TLS_LDFLAGS) -fsanitize=fuzzer,address,undefined
fuzz-parser: clean $(LIB_OBJS) $(FUZZ_PARSER_OBJ)
	$(CC) $(CFLAGS) -o $(FUZZ_PARSER_TARGET) $(LIB_OBJS) $(FUZZ_PARSER_OBJ) $(LDFLAGS)
	@mkdir -p fuzz/corpus_parser
	@echo "\n==> Parser fuzzer built: $(FUZZ_PARSER_TARGET)"
	@echo "    Run:  $(FUZZ_PARSER_TARGET) fuzz/corpus_parser/ -max_len=4096"

fuzz-all: CC = $(FUZZ_CC)
fuzz-all: CFLAGS = -std=c11 -D_GNU_SOURCE -Iinclude $(EDIT_CFLAGS) $(TLS_CFLAGS) -fsanitize=fuzzer,address,undefined -g -O1
fuzz-all: LDFLAGS = $(EDIT_LDFLAGS) $(TLS_LDFLAGS) -fsanitize=fuzzer,address,undefined
fuzz-all: clean $(LIB_OBJS) $(FUZZ_OBJ) $(FUZZ_VM_OBJ) $(FUZZ_STACKVM_OBJ) $(FUZZ_REGVM_OBJ) $(FUZZ_LATC_OBJ) $(FUZZ_JSON_OBJ) $(FUZZ_TOML_OBJ) $(FUZZ_YAML_OBJ) $(FUZZ_LEXER_OBJ) $(FUZZ_FORMATTER_OBJ)
	$(CC) $(CFLAGS) -o $(FUZZ_TARGET) $(LIB_OBJS) $(FUZZ_OBJ) $(LDFLAGS)
	$(CC) $(CFLAGS) -o $(FUZZ_VM_TARGET) $(LIB_OBJS) $(FUZZ_VM_OBJ) $(LDFLAGS)
	$(CC) $(CFLAGS) -o $(FUZZ_STACKVM_TARGET) $(LIB_OBJS) $(FUZZ_STACKVM_OBJ) $(LDFLAGS)
	$(CC) $(CFLAGS) -o $(FUZZ_REGVM_TARGET) $(LIB_OBJS) $(FUZZ_REGVM_OBJ) $(LDFLAGS)
	$(CC) $(CFLAGS) -o $(FUZZ_LATC_TARGET) $(LIB_OBJS) $(FUZZ_LATC_OBJ) $(LDFLAGS)
	$(CC) $(CFLAGS) -o $(FUZZ_JSON_TARGET) $(LIB_OBJS) $(FUZZ_JSON_OBJ) $(LDFLAGS)
	$(CC) $(CFLAGS) -o $(FUZZ_TOML_TARGET) $(LIB_OBJS) $(FUZZ_TOML_OBJ) $(LDFLAGS)
	$(CC) $(CFLAGS) -o $(FUZZ_YAML_TARGET) $(LIB_OBJS) $(FUZZ_YAML_OBJ) $(LDFLAGS)
	$(CC) $(CFLAGS) -o $(FUZZ_LEXER_TARGET) $(LIB_OBJS) $(FUZZ_LEXER_OBJ) $(LDFLAGS)
	$(CC) $(CFLAGS) -o $(FUZZ_FORMATTER_TARGET) $(LIB_OBJS) $(FUZZ_FORMATTER_OBJ) $(LDFLAGS)
	@mkdir -p fuzz/corpus fuzz/corpus_latc fuzz/corpus_json fuzz/corpus_toml fuzz/corpus_yaml
	@echo "\n==> All fuzzers built:"
	@echo "    $(FUZZ_TARGET)         (tree-walk evaluator)"
	@echo "    $(FUZZ_VM_TARGET)           (stack VM + RegVM combined)"
	@echo "    $(FUZZ_STACKVM_TARGET)     (stack VM standalone)"
	@echo "    $(FUZZ_REGVM_TARGET)       (RegVM standalone)"
	@echo "    $(FUZZ_LATC_TARGET)         (bytecode deserializer)"
	@echo "    $(FUZZ_JSON_TARGET)         (JSON parser)"
	@echo "    $(FUZZ_TOML_TARGET)         (TOML parser)"
	@echo "    $(FUZZ_YAML_TARGET)         (YAML parser)"
	@echo "    $(FUZZ_LEXER_TARGET)       (lexer/tokenizer)"
	@echo "    $(FUZZ_FORMATTER_TARGET) (source formatter)"

FUZZ_EXCLUDE = http_server http_client https_client tls_client orm_demo
fuzz-seed:
	@mkdir -p fuzz/corpus fuzz/corpus_json fuzz/corpus_toml fuzz/corpus_yaml
	@cp -v examples/*.lat fuzz/corpus/ 2>/dev/null || true
	@cp -v benchmarks/*.lat fuzz/corpus/ 2>/dev/null || true
	@cp -v fuzz/seeds/*.lat fuzz/corpus/ 2>/dev/null || true
	@for f in $(FUZZ_EXCLUDE); do rm -f fuzz/corpus/$$f.lat; done
	@cp -v fuzz/seeds_json/*.json fuzz/corpus_json/ 2>/dev/null || true
	@cp -v fuzz/seeds_toml/*.toml fuzz/corpus_toml/ 2>/dev/null || true
	@cp -v fuzz/seeds_yaml/*.yaml fuzz/corpus_yaml/ 2>/dev/null || true
	@echo "\n==> Corpus seeded:"
	@echo "    Lattice: $$(ls fuzz/corpus/*.lat 2>/dev/null | wc -l | tr -d ' ') files (excluded: $(FUZZ_EXCLUDE))"
	@echo "    JSON:    $$(ls fuzz/corpus_json/* 2>/dev/null | wc -l | tr -d ' ') files"
	@echo "    TOML:    $$(ls fuzz/corpus_toml/* 2>/dev/null | wc -l | tr -d ' ') files"
	@echo "    YAML:    $$(ls fuzz/corpus_yaml/* 2>/dev/null | wc -l | tr -d ' ') files"

wasm:
	emcc $(WASM_FLAGS) -o $(WASM_OUT) $(WASM_SRCS)

bench: $(TARGET)
	@for f in benchmarks/*.lat; do echo "--- $$f ---"; ./clat --stats "$$f"; done

bench-regvm: $(TARGET)
	@for f in benchmarks/*.lat; do echo "--- $$f (regvm) ---"; ./clat --regvm --stats "$$f"; done

bench-stress: $(TARGET)
	@for f in benchmarks/*.lat; do echo "--- $$f ---"; ./clat --gc-stress --stats "$$f"; done

bench-all: $(TARGET)
	@bash scripts/run-benchmarks.sh

# Crystal-by-Reference Stage 3 smoke gate: fix-binding freeze of a 10k-element
# array x1000 iterations on the default stack VM must stay within a generous
# multiple of the pre-Stage-3 baseline (baked into the script). Catches
# accidental O(n^2) freeze behavior, not microbenchmark noise.
bench-freeze-gate: $(TARGET)
	python3 scripts/bench_freeze_gate.py

# Crystal-by-Reference Stage 6 (LAT-459) accounting suite: read/alias/arg-pass/
# channel/spawn/freeze-loop workloads, normal vs LATTICE_FORCE_COPY=1, on all
# three backends, plus the value_worth_regionizing threshold sweep. Writes
# benchmarks/RESULTS.md.
bench-cbr: $(TARGET)
	python3 scripts/bench_cbr.py

ext-pg:
	$(MAKE) -C extensions/pg

ext-sqlite:
	$(MAKE) -C extensions/sqlite

ext-ffi:
	$(MAKE) -C extensions/ffi

ext-redis:
	$(MAKE) -C extensions/redis

ext-websocket:
	$(MAKE) -C extensions/websocket

ext-image:
	$(MAKE) -C extensions/image

SITE_DIR = ../lattice-lang.org

deploy-coverage: coverage
	rm -rf $(SITE_DIR)/coverage
	cp -r $(BUILD_DIR)/coverage $(SITE_DIR)/coverage
	@VERSION=$$(grep 'LATTICE_VERSION' include/lattice.h | head -1 | sed 's/.*"\(.*\)".*/\1/'); \
	SRC_LINES=$$(cat $(LIB_SRCS) include/*.h | wc -l | tr -d ' '); \
	TEST_MACRO=$$(grep -c '^[[:space:]]*TEST(' tests/test_*.c | awk -F: '{s+=$$2} END{print s}'); \
	TEST_REG=$$(grep -c 'register_test("' tests/test_*.c | awk -F: '{s+=$$2} END{print s}'); \
	TEST_COUNT=$$((TEST_MACRO + TEST_REG)); \
	sed -i '' "s/__VERSION__/v$$VERSION/g; s/__SRC_LINES__/$$SRC_LINES/g; s/__TEST_COUNT__/$$TEST_COUNT/g" $(SITE_DIR)/dev.html; \
	echo ""; \
	echo "==> Coverage deployed to $(SITE_DIR)/coverage/"; \
	echo "    Stats updated in $(SITE_DIR)/dev.html"; \
	cd $(SITE_DIR) && firebase deploy; \
	cd - > /dev/null; \
	sed -i '' "s/v$$VERSION/__VERSION__/g; s/$$SRC_LINES/__SRC_LINES__/g; s/$$TEST_COUNT/__TEST_COUNT__/g" $(SITE_DIR)/dev.html; \
	echo "    Placeholders restored in dev.html"

# ── Install ──
PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin

install: $(TARGET)
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET)

# ── Release binary (current platform) ──
ifndef WINDOWS
UNAME_M := $(shell uname -m)
RELEASE_ARCH := $(if $(filter arm64,$(UNAME_M)),aarch64,$(UNAME_M))
RELEASE_OS := $(if $(filter Darwin,$(UNAME_S)),darwin,$(if $(filter Linux,$(UNAME_S)),linux,$(if $(filter FreeBSD,$(UNAME_S)),freebsd,$(if $(filter OpenBSD,$(UNAME_S)),openbsd,$(if $(filter NetBSD,$(UNAME_S)),netbsd,unknown)))))
RELEASE_NAME := clat-$(RELEASE_OS)-$(RELEASE_ARCH)
endif

ifdef WINDOWS
STRIP = x86_64-w64-mingw32-strip
RUNTIME_RELEASE_NAME = clat-run-windows-x86_64.exe
else
STRIP = strip
RUNTIME_RELEASE_NAME = clat-run-$(RELEASE_OS)-$(RELEASE_ARCH)
endif

release: clean $(TARGET)
	$(STRIP) $(TARGET)
	cp $(TARGET) $(RELEASE_NAME)
	@echo "Built: $(RELEASE_NAME) ($$(ls -lh $(RELEASE_NAME) | awk '{print $$5}'))"

runtime-release: clean $(RUNTIME_TARGET)
	$(STRIP) $(RUNTIME_TARGET)
	cp $(RUNTIME_TARGET) $(RUNTIME_RELEASE_NAME)
	@echo "Built: $(RUNTIME_RELEASE_NAME) ($$(ls -lh $(RUNTIME_RELEASE_NAME) | awk '{print $$5}'))"

clean:
	rm -rf $(BUILD_DIR) $(TARGET) $(LSP_TARGET) $(RUNTIME_TARGET)
	rm -f clat-run-* *.plist src/*.plist
