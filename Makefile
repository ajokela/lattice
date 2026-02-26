CC      = cc
CFLAGS  = -std=c11 -Wall -Wextra -Werror -Iinclude -O3 -DVM_USE_COMPUTED_GOTO
LDFLAGS =

SRC_DIR    = src
BUILD_DIR  = build
TEST_DIR   = tests

# ── Optional libedit / readline ──
EDIT_AVAILABLE := $(shell pkg-config --exists libedit 2>/dev/null && echo yes || echo no)
ifeq ($(EDIT_AVAILABLE),yes)
    EDIT_CFLAGS  := $(shell pkg-config --cflags libedit) -DLATTICE_HAS_EDITLINE
    EDIT_LDFLAGS := $(shell pkg-config --libs libedit)
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
    TLS_LDFLAGS := $(shell pkg-config --libs openssl)
endif
ifeq ($(TLS),0)
    TLS_CFLAGS  :=
    TLS_LDFLAGS :=
endif
CFLAGS  += $(TLS_CFLAGS)
LDFLAGS += $(TLS_LDFLAGS)

# ── pthreads and dlopen (Linux needs explicit linking) ──
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
    CFLAGS  += -D_DEFAULT_SOURCE
    LDFLAGS += -lpthread -lm -ldl -rdynamic
endif

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
       $(SRC_DIR)/completion.c \
       $(SRC_DIR)/doc_gen.c \
       $(SRC_DIR)/iterator.c \
       $(SRC_DIR)/gc.c

# All source files except main.c (for tests)
LIB_SRCS = $(filter-out $(SRC_DIR)/main.c, $(SRCS))

OBJS     = $(SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)
LIB_OBJS = $(LIB_SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)

TARGET = clat

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
            $(TEST_DIR)/test_latc.c \
            $(TEST_DIR)/test_debugger.c

TEST_OBJS   = $(TEST_SRCS:$(TEST_DIR)/%.c=$(BUILD_DIR)/tests/%.o)
TEST_TARGET = $(BUILD_DIR)/test_runner

# WASM build
WASM_SRCS   = $(LIB_SRCS) $(SRC_DIR)/wasm_api.c
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

# VM/RegVM fuzz harness
FUZZ_VM_SRC    = $(FUZZ_DIR)/fuzz_vm.c
FUZZ_VM_OBJ    = $(BUILD_DIR)/fuzz/fuzz_vm.o
FUZZ_VM_TARGET = $(BUILD_DIR)/fuzz_vm

.PHONY: all clean test test-tree-walk test-regvm test-all-backends test-latc asan asan-all tsan coverage analyze clang-tidy fuzz fuzz-latc fuzz-vm fuzz-seed wasm bench bench-regvm bench-stress ext-pg ext-sqlite ext-ffi ext-redis ext-websocket ext-image lsp deploy-coverage

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

# LSP server
lsp: $(LSP_TARGET)

$(LSP_TARGET): $(LIB_OBJS) $(LSP_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(BUILD_DIR)/vendor/%.o: vendor/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Wno-unused-parameter -c -o $@ $<

$(BUILD_DIR)/tests/%.o: $(TEST_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

$(TEST_TARGET): $(LIB_OBJS) $(LSP_LIB_OBJS) $(TEST_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test: $(TEST_TARGET)
	./$(BUILD_DIR)/test_runner

test-tree-walk: $(TEST_TARGET)
	./$(BUILD_DIR)/test_runner --backend tree-walk

test-regvm: $(TEST_TARGET)
	./$(BUILD_DIR)/test_runner --backend regvm

test-all-backends: $(TEST_TARGET)
	@echo "=== stack-vm ===" && ./$(BUILD_DIR)/test_runner --backend stack-vm
	@echo "=== tree-walk ===" && ./$(BUILD_DIR)/test_runner --backend tree-walk
	@echo "=== regvm ===" && ./$(BUILD_DIR)/test_runner --backend regvm

LATC_TESTS = latc_roundtrip latc_structs latc_match latc_new_features latc_traits latc_traits_codegen

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
		./$(TARGET) /tmp/$$name.latc > /tmp/$$name.compiled.out 2>&1; \
		if [ $$? -ne 0 ]; then \
			echo "FAIL (run .latc)"; \
			cat /tmp/$$name.compiled.out; \
			FAIL=$$((FAIL + 1)); \
			rm -f /tmp/$$name.latc /tmp/$$name.compiled.out; \
			continue; \
		fi; \
		./$(TARGET) tests/$$name.lat > /tmp/$$name.direct.out 2>&1; \
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
	echo "Results: $$PASS passed, $$FAIL failed"; \
	if [ $$FAIL -gt 0 ]; then exit 1; fi

asan: CFLAGS += -fsanitize=address,undefined -g -O1
asan: LDFLAGS += -fsanitize=address,undefined
asan: clean $(TEST_TARGET)
	./$(BUILD_DIR)/test_runner

asan-all: CFLAGS += -fsanitize=address,undefined -g -O1
asan-all: LDFLAGS += -fsanitize=address,undefined
asan-all: clean $(TEST_TARGET)
	@echo "=== asan: stack-vm ===" && ./$(BUILD_DIR)/test_runner --backend stack-vm
	@echo "=== asan: tree-walk ===" && ./$(BUILD_DIR)/test_runner --backend tree-walk
	@echo "=== asan: regvm ===" ; ./$(BUILD_DIR)/test_runner --backend regvm; \
	 rc=$$?; if [ $$rc -gt 128 ]; then exit $$rc; fi

tsan: CFLAGS += -fsanitize=thread -g -O1
tsan: LDFLAGS += -fsanitize=thread
tsan: clean $(TEST_TARGET)
	./$(BUILD_DIR)/test_runner

coverage: CFLAGS += -fprofile-instr-generate -fcoverage-mapping -g -O0
coverage: LDFLAGS += -fprofile-instr-generate
coverage: clean $(TEST_TARGET)
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
		clang-tidy $$f -- -std=c11 -Iinclude $(EDIT_CFLAGS) $(TLS_CFLAGS) -D_DEFAULT_SOURCE 2>&1 || FAIL=1; \
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
fuzz: CFLAGS = -std=c11 -D_DEFAULT_SOURCE -Iinclude $(EDIT_CFLAGS) $(TLS_CFLAGS) -fsanitize=fuzzer,address,undefined -g -O1
fuzz: LDFLAGS = $(EDIT_LDFLAGS) $(TLS_LDFLAGS) -fsanitize=fuzzer,address,undefined
fuzz: clean $(LIB_OBJS) $(FUZZ_OBJ)
	$(CC) $(CFLAGS) -o $(FUZZ_TARGET) $(LIB_OBJS) $(FUZZ_OBJ) $(LDFLAGS)
	@mkdir -p fuzz/corpus
	@echo "\n==> Fuzzer built: $(FUZZ_TARGET)"
	@echo "    Run:  $(FUZZ_TARGET) fuzz/corpus/ -max_len=4096"
	@echo "    Seed: cp examples/*.lat benchmarks/*.lat fuzz/corpus/"

fuzz-latc: CC = $(FUZZ_CC)
fuzz-latc: CFLAGS = -std=c11 -D_DEFAULT_SOURCE -Iinclude $(EDIT_CFLAGS) $(TLS_CFLAGS) -fsanitize=fuzzer,address,undefined -g -O1
fuzz-latc: LDFLAGS = $(EDIT_LDFLAGS) $(TLS_LDFLAGS) -fsanitize=fuzzer,address,undefined
fuzz-latc: clean $(LIB_OBJS) $(FUZZ_LATC_OBJ)
	$(CC) $(CFLAGS) -o $(FUZZ_LATC_TARGET) $(LIB_OBJS) $(FUZZ_LATC_OBJ) $(LDFLAGS)
	@mkdir -p fuzz/corpus_latc
	@echo "\n==> Bytecode fuzzer built: $(FUZZ_LATC_TARGET)"
	@echo "    Run:  $(FUZZ_LATC_TARGET) fuzz/corpus_latc/ -max_len=65536"

fuzz-vm: CC = $(FUZZ_CC)
fuzz-vm: CFLAGS = -std=c11 -D_DEFAULT_SOURCE -Iinclude $(EDIT_CFLAGS) $(TLS_CFLAGS) -fsanitize=fuzzer,address,undefined -g -O1
fuzz-vm: LDFLAGS = $(EDIT_LDFLAGS) $(TLS_LDFLAGS) -fsanitize=fuzzer,address,undefined
fuzz-vm: clean $(LIB_OBJS) $(FUZZ_VM_OBJ)
	$(CC) $(CFLAGS) -o $(FUZZ_VM_TARGET) $(LIB_OBJS) $(FUZZ_VM_OBJ) $(LDFLAGS)
	@mkdir -p fuzz/corpus
	@echo "\n==> VM fuzzer built: $(FUZZ_VM_TARGET)"
	@echo "    Run:  $(FUZZ_VM_TARGET) fuzz/corpus/ -max_len=4096"
	@echo "    Seed: make fuzz-seed"

FUZZ_EXCLUDE = http_server http_client https_client tls_client orm_demo
fuzz-seed:
	@mkdir -p fuzz/corpus
	@cp -v examples/*.lat fuzz/corpus/ 2>/dev/null || true
	@cp -v benchmarks/*.lat fuzz/corpus/ 2>/dev/null || true
	@cp -v fuzz/seeds/*.lat fuzz/corpus/ 2>/dev/null || true
	@for f in $(FUZZ_EXCLUDE); do rm -f fuzz/corpus/$$f.lat; done
	@echo "\n==> Corpus seeded with $$(ls fuzz/corpus/*.lat 2>/dev/null | wc -l | tr -d ' ') files (excluded: $(FUZZ_EXCLUDE))"

wasm:
	emcc $(WASM_FLAGS) -o $(WASM_OUT) $(WASM_SRCS)

bench: $(TARGET)
	@for f in benchmarks/*.lat; do echo "--- $$f ---"; ./clat --stats "$$f"; done

bench-regvm: $(TARGET)
	@for f in benchmarks/*.lat; do echo "--- $$f (regvm) ---"; ./clat --regvm --stats "$$f"; done

bench-stress: $(TARGET)
	@for f in benchmarks/*.lat; do echo "--- $$f ---"; ./clat --gc-stress --stats "$$f"; done

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

clean:
	rm -rf $(BUILD_DIR) $(TARGET) $(LSP_TARGET)
	rm -f *.plist src/*.plist
