CC     = cc
CFLAGS = -std=c11 -Wall -Wextra -Werror -fPIC -I../../include

# PostgreSQL flags via pg_config (only include path, not compiler flags)
PG_INCLUDE = -I$(shell pg_config --includedir 2>/dev/null)
PG_LIBDIR  = $(shell pg_config --libdir 2>/dev/null)

CFLAGS += $(PG_INCLUDE)

# Platform-specific shared library settings
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
    SHARED_EXT = .dylib
    SHARED_FLAGS = -dynamiclib -undefined dynamic_lookup
else
    SHARED_EXT = .so
    SHARED_FLAGS = -shared
endif

TARGET = pg$(SHARED_EXT)

.PHONY: all clean install

all: $(TARGET)

$(TARGET): pg.c
	$(CC) $(CFLAGS) $(SHARED_FLAGS) -o $@ $< -L$(PG_LIBDIR) -lpq

clean:
	rm -f $(TARGET)

# Install to ~/.lattice/ext/
install: $(TARGET)
	@mkdir -p $(HOME)/.lattice/ext
	cp $(TARGET) $(HOME)/.lattice/ext/
