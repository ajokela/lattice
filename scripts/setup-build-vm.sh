#!/bin/sh
# Bootstrap a BSD build VM for Lattice release builds.
# Run this on a fresh FreeBSD, OpenBSD, or NetBSD VM.
# Usage: ssh freebsd-amd64 'sh -s' < scripts/setup-build-vm.sh
set -eu

REPO_URL="https://github.com/alexjokela/lattice.git"
CLONE_DIR="$HOME/lattice"

say()  { printf "\033[1;34m=>\033[0m %s\n" "$*"; }
err()  { printf "\033[1;31merror:\033[0m %s\n" "$*" >&2; exit 1; }
ok()   { printf "\033[1;32mok:\033[0m %s\n" "$*"; }

OS=$(uname -s)
say "Detected OS: $OS"

# ── Install packages ────────────────────────────────────────────────────────

case "$OS" in
    FreeBSD)
        say "Installing packages (FreeBSD)..."
        sudo pkg install -y git gmake
        # OpenSSL and libedit are in base
        ok "Packages installed"
        ;;
    OpenBSD)
        say "Installing packages (OpenBSD)..."
        sudo pkg_add git gmake
        # LibreSSL and libedit are in base
        ok "Packages installed"
        ;;
    NetBSD)
        say "Installing packages (NetBSD)..."
        sudo pkgin -y install git gmake openssl libedit
        ok "Packages installed"
        ;;
    *)
        err "Unsupported OS: $OS (expected FreeBSD, OpenBSD, or NetBSD)"
        ;;
esac

# ── Clone repository ────────────────────────────────────────────────────────

if [ -d "$CLONE_DIR" ]; then
    say "Repository already exists at $CLONE_DIR, updating..."
    cd "$CLONE_DIR"
    git fetch --all
else
    say "Cloning repository..."
    git clone "$REPO_URL" "$CLONE_DIR"
    cd "$CLONE_DIR"
fi

# ── Test build ──────────────────────────────────────────────────────────────

say "Testing build..."
gmake clean
gmake

LATTICE_VERSION=$("./clat" --version 2>&1 | head -1 || echo "unknown")
ok "Build successful: $LATTICE_VERSION"

say "Testing release build..."
gmake release
RELEASE_BIN=$(ls clat-* 2>/dev/null | head -1)
if [ -n "$RELEASE_BIN" ]; then
    ok "Release binary: $RELEASE_BIN ($(ls -lh "$RELEASE_BIN" | awk '{print $5}'))"
    rm -f "$RELEASE_BIN"
else
    err "Release binary not found"
fi

echo ""
say "VM is ready for release builds!"
