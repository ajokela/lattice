CC      = cc
CFLAGS  = -std=c11 -Wall -Wextra -Werror -Iinclude
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
    LDFLAGS += -lpthread -lm -ldl
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
       $(SRC_DIR)/ext.c

# All source files except main.c (for tests)
LIB_SRCS = $(filter-out $(SRC_DIR)/main.c, $(SRCS))

OBJS     = $(SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)
LIB_OBJS = $(LIB_SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)

TARGET = clat

# Test sources
TEST_SRCS = $(TEST_DIR)/test_main.c \
            $(TEST_DIR)/test_ds.c \
            $(TEST_DIR)/test_memory.c \
            $(TEST_DIR)/test_eval.c \
            $(TEST_DIR)/test_stdlib.c

TEST_OBJS   = $(TEST_SRCS:$(TEST_DIR)/%.c=$(BUILD_DIR)/tests/%.o)
TEST_TARGET = $(BUILD_DIR)/test_runner

# WASM build
WASM_SRCS   = $(LIB_SRCS) $(SRC_DIR)/wasm_api.c
WASM_OUT    = lattice-lang.org/lattice.js
WASM_FLAGS  = -std=gnu11 -Wall -Wextra -Wno-error -Wno-constant-conversion -Iinclude -O2 \
              -sEXPORTED_FUNCTIONS=_lat_init,_lat_run_line,_lat_is_complete,_lat_destroy,_free \
              -sEXPORTED_RUNTIME_METHODS=ccall,cwrap,UTF8ToString \
              -sMODULARIZE=1 -sEXPORT_NAME=createLattice \
              -sALLOW_MEMORY_GROWTH=1

.PHONY: all clean test asan wasm bench bench-stress ext-pg ext-sqlite

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD_DIR)/tests/%.o: $(TEST_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

$(TEST_TARGET): $(LIB_OBJS) $(TEST_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

test: $(TEST_TARGET)
	./$(BUILD_DIR)/test_runner

asan: CFLAGS += -fsanitize=address,undefined -g -O1
asan: LDFLAGS += -fsanitize=address,undefined
asan: clean $(TEST_TARGET)
	./$(BUILD_DIR)/test_runner

wasm:
	emcc $(WASM_FLAGS) -o $(WASM_OUT) $(WASM_SRCS)

bench: $(TARGET)
	@for f in benchmarks/*.lat; do echo "--- $$f ---"; ./clat --stats "$$f"; done

bench-stress: $(TARGET)
	@for f in benchmarks/*.lat; do echo "--- $$f ---"; ./clat --gc-stress --stats "$$f"; done

ext-pg:
	$(MAKE) -C extensions/pg

ext-sqlite:
	$(MAKE) -C extensions/sqlite

clean:
	rm -rf $(BUILD_DIR) $(TARGET)
