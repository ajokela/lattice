CC     = cc
CFLAGS = -std=c11 -Wall -Wextra -Werror -fPIC -I../../include

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
    SHARED_EXT = .dylib
    SHARED_FLAGS = -dynamiclib -undefined dynamic_lookup
    LDFLAGS =
else
    SHARED_EXT = .so
    SHARED_FLAGS = -shared
    LDFLAGS = -ldl
endif

TARGET = ffi$(SHARED_EXT)

.PHONY: all clean install

all: $(TARGET)

$(TARGET): ffi.c
	$(CC) $(CFLAGS) $(SHARED_FLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(TARGET)

install: $(TARGET)
	@mkdir -p $(HOME)/.lattice/ext
	cp $(TARGET) $(HOME)/.lattice/ext/
