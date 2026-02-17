CC     = cc
CFLAGS = -std=c11 -Wall -Wextra -Werror -fPIC -I../../include

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
    SHARED_EXT = .dylib
    SHARED_FLAGS = -dynamiclib -undefined dynamic_lookup
else
    SHARED_EXT = .so
    SHARED_FLAGS = -shared
endif

# OpenSSL for SHA1 in handshake
OPENSSL_CFLAGS := $(shell pkg-config --cflags openssl 2>/dev/null)
OPENSSL_LIBS   := $(shell pkg-config --libs openssl 2>/dev/null)

TARGET = websocket$(SHARED_EXT)

.PHONY: all clean install

all: $(TARGET)

$(TARGET): websocket.c
	$(CC) $(CFLAGS) $(OPENSSL_CFLAGS) $(SHARED_FLAGS) -o $@ $< $(OPENSSL_LIBS)

clean:
	rm -f $(TARGET)

install: $(TARGET)
	@mkdir -p $(HOME)/.lattice/ext
	cp $(TARGET) $(HOME)/.lattice/ext/
