CC     = cc
CFLAGS = -std=c11 -Wall -Wextra -Werror -fPIC -I../../include

# Platform-specific shared library settings
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
    SHARED_EXT = .dylib
    SHARED_FLAGS = -dynamiclib -undefined dynamic_lookup
else
    SHARED_EXT = .so
    SHARED_FLAGS = -shared
endif

TARGET = redis$(SHARED_EXT)

.PHONY: all clean install

all: $(TARGET)

$(TARGET): redis.c
	$(CC) $(CFLAGS) $(SHARED_FLAGS) -o $@ $<

clean:
	rm -f $(TARGET)

# Install to ~/.lattice/ext/
install: $(TARGET)
	@mkdir -p $(HOME)/.lattice/ext
	cp $(TARGET) $(HOME)/.lattice/ext/
