#!/bin/sh
# Portable BSD build script — uses cc directly, no gmake/GNU extensions needed.
# Designed to run on FreeBSD, OpenBSD, and NetBSD with only base system tools.
# Usage: sh build-bsd.sh
set -eu

OS=$(uname -s)
ARCH=$(uname -m)
case "$ARCH" in
    amd64|x86_64)  RELEASE_ARCH="x86_64"  ;;
    aarch64|arm64|evbarm) RELEASE_ARCH="aarch64" ;;
    *)              RELEASE_ARCH="$ARCH"   ;;
esac
case "$OS" in
    FreeBSD) RELEASE_OS="freebsd" ;;
    OpenBSD) RELEASE_OS="openbsd" ;;
    NetBSD)  RELEASE_OS="netbsd"  ;;
    *)       echo "error: unsupported OS: $OS" >&2; exit 1 ;;
esac
RELEASE_NAME="clat-${RELEASE_OS}-${RELEASE_ARCH}"

echo "==> Building $RELEASE_NAME on $OS $ARCH"

CC="${CC:-cc}"
CFLAGS="-std=c11 -Wall -Wextra -Werror -Iinclude -O3"
LDFLAGS=""

# GCC (common on aarch64 BSDs) has stricter -Wimplicit-fallthrough than clang
# Detect GCC by absence of "clang" in version output
if ! $CC --version 2>&1 | grep -qi clang; then
    CFLAGS="$CFLAGS -Wno-implicit-fallthrough"
fi

# Platform-specific flags
case "$OS" in
    FreeBSD)
        # OpenSSL and libedit in base; readline compat headers at /usr/include/edit/readline/
        CFLAGS="$CFLAGS -DLATTICE_HAS_TLS -DLATTICE_HAS_READLINE -I/usr/include/edit"
        LDFLAGS="-ledit -lssl -lcrypto -lpthread -lm"
        ;;
    OpenBSD)
        # LibreSSL in base; libedit with readline compat headers at /usr/include/readline/
        # -include stdio.h needed because OpenBSD's readline.h uses FILE* without including it
        CFLAGS="$CFLAGS -DLATTICE_HAS_TLS -DLATTICE_HAS_READLINE -include stdio.h"
        LDFLAGS="-lreadline -lssl -lcrypto -lpthread -lm"
        ;;
    NetBSD)
        # OpenSSL and libedit in base; readline compat headers, linked as -ledit
        CFLAGS="$CFLAGS -DLATTICE_HAS_TLS -DLATTICE_HAS_READLINE"
        LDFLAGS="-ledit -lssl -lcrypto -lpthread -lm"
        # pkgsrc paths if needed
        [ -d /usr/pkg/include ] && CFLAGS="$CFLAGS -I/usr/pkg/include" && LDFLAGS="-L/usr/pkg/lib $LDFLAGS"
        ;;
esac

# Collect source files
SRCS=""
for f in \
    src/main.c src/ds/str.c src/ds/vec.c src/ds/hashmap.c \
    src/token.c src/lexer.c src/ast.c src/parser.c src/value.c src/env.c \
    src/eval.c src/memory.c src/phase_check.c src/string_ops.c src/builtins.c \
    src/net.c src/tls.c src/json.c src/math_ops.c src/env_ops.c src/time_ops.c \
    src/fs_ops.c src/process_ops.c src/type_ops.c src/datetime_ops.c \
    src/regex_ops.c src/format_ops.c src/path_ops.c src/crypto_ops.c \
    src/array_ops.c src/channel.c src/http.c src/toml_ops.c src/yaml_ops.c \
    src/ext.c src/stackopcode.c src/chunk.c src/stackcompiler.c src/stackvm.c \
    src/runtime.c src/intern.c src/latc.c src/regopcode.c src/regcompiler.c \
    src/regvm.c src/builtin_methods.c src/match_check.c src/package.c \
    src/formatter.c src/debugger.c src/completion.c src/doc_gen.c \
    src/iterator.c src/gc.c; do
    SRCS="$SRCS $f"
done

# Build
rm -rf build clat
mkdir -p build/ds

echo "==> Compiling..."
OBJS=""
for f in $SRCS; do
    obj="build/$(echo "$f" | sed 's|^src/||; s|\.c$|.o|')"
    mkdir -p "$(dirname "$obj")"
    $CC $CFLAGS -c -o "$obj" "$f"
    OBJS="$OBJS $obj"
done

echo "==> Linking..."
$CC $CFLAGS -o clat $OBJS $LDFLAGS

echo "==> Stripping..."
strip clat
cp clat "$RELEASE_NAME"

SIZE=$(ls -lh "$RELEASE_NAME" | awk '{print $5}')
echo "==> Built: $RELEASE_NAME ($SIZE)"
