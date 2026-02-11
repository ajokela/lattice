CC      = cc
CFLAGS  = -std=c11 -Wall -Wextra -Werror -Iinclude
LDFLAGS = -ledit

SRC_DIR    = src
BUILD_DIR  = build
TEST_DIR   = tests

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
       $(SRC_DIR)/builtins.c

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

.PHONY: all clean test asan wasm bench bench-stress

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

clean:
	rm -rf $(BUILD_DIR) $(TARGET)
