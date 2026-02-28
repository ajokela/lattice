#!/bin/sh
# Lattice installer — curl -fsSL https://lattice-lang.org/install.sh | sh
set -eu

LATTICE_INSTALL_URL="${LATTICE_INSTALL_URL:-https://lattice-lang.org}"
GITHUB_RELEASE_URL="https://github.com/alexjokela/lattice/releases/download"

# ── Helpers ──────────────────────────────────────────────────────────────────

say()  { printf "  %s\n" "$@"; }
err()  { printf "error: %s\n" "$@" >&2; exit 1; }
bold() { printf "\033[1m%s\033[0m\n" "$@"; }

need() {
    command -v "$1" >/dev/null 2>&1 || err "$1 is required but not found"
}

# Download a URL to stdout. Tries curl first, then wget.
fetch() {
    if command -v curl >/dev/null 2>&1; then
        curl -fsSL "$1"
    elif command -v wget >/dev/null 2>&1; then
        wget -qO- "$1"
    else
        err "curl or wget is required"
    fi
}

# Download a URL to a file. Tries curl first, then wget.
download() {
    if command -v curl >/dev/null 2>&1; then
        curl -fsSL -o "$2" "$1"
    elif command -v wget >/dev/null 2>&1; then
        wget -qO "$2" "$1"
    else
        err "curl or wget is required"
    fi
}

# SHA256 checksum of a file (portable across platforms)
sha256() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    elif command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$1" | awk '{print $1}'
    elif command -v openssl >/dev/null 2>&1; then
        openssl dgst -sha256 "$1" | awk '{print $NF}'
    else
        say "warning: no sha256 tool found, skipping checksum verification"
        echo ""
    fi
}

# ── Detect platform ─────────────────────────────────────────────────────────

detect_os() {
    case "$(uname -s)" in
        Darwin)  echo "darwin"  ;;
        Linux)   echo "linux"   ;;
        FreeBSD) echo "freebsd" ;;
        OpenBSD) echo "openbsd" ;;
        NetBSD)  echo "netbsd"  ;;
        *)       err "unsupported OS: $(uname -s)" ;;
    esac
}

detect_arch() {
    case "$(uname -m)" in
        x86_64|amd64)  echo "x86_64"  ;;
        aarch64|arm64) echo "aarch64" ;;
        riscv64)       echo "riscv64" ;;
        *)             err "unsupported architecture: $(uname -m)" ;;
    esac
}

# ── Resolve version ─────────────────────────────────────────────────────────

resolve_version() {
    if [ -n "${VERSION:-}" ]; then
        echo "$VERSION"
        return
    fi

    # Parse -s -- vX.Y.Z from args (used by curl | sh -s -- v0.3.27)
    for arg in "$@"; do
        case "$arg" in
            v*) echo "$arg"; return ;;
        esac
    done

    # Fetch latest version from website
    say "Fetching latest version..."
    LATEST_JSON=$(fetch "$LATTICE_INSTALL_URL/releases/latest.json") || \
        err "failed to fetch latest version from $LATTICE_INSTALL_URL/releases/latest.json"
    # Parse version from JSON (portable — no jq dependency)
    echo "$LATEST_JSON" | sed 's/.*"version"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/'
}

# ── Main ─────────────────────────────────────────────────────────────────────

main() {
    OS=$(detect_os)
    ARCH=$(detect_arch)
    VERSION=$(resolve_version "$@")
    BINARY="clat-${OS}-${ARCH}"

    bold "Installing Lattice $VERSION ($OS/$ARCH)"
    echo ""

    # Create temp directory
    TMPDIR=$(mktemp -d)
    trap 'rm -rf "$TMPDIR"' EXIT

    # Try lattice-lang.org first, fall back to GitHub Releases
    DOWNLOADED=false
    for BASE_URL in \
        "$LATTICE_INSTALL_URL/releases/$VERSION" \
        "$GITHUB_RELEASE_URL/$VERSION"; do

        say "Downloading from $BASE_URL..."
        if download "$BASE_URL/$BINARY" "$TMPDIR/clat" 2>/dev/null; then
            # Also grab checksums for verification
            download "$BASE_URL/checksums.txt" "$TMPDIR/checksums.txt" 2>/dev/null || true
            DOWNLOADED=true
            break
        fi
    done

    if [ "$DOWNLOADED" = "false" ]; then
        err "failed to download $BINARY for $VERSION from any source"
    fi

    # Verify checksum if checksums.txt was downloaded
    if [ -f "$TMPDIR/checksums.txt" ]; then
        EXPECTED=$(grep "$BINARY" "$TMPDIR/checksums.txt" | awk '{print $1}')
        if [ -n "$EXPECTED" ]; then
            ACTUAL=$(sha256 "$TMPDIR/clat")
            if [ -n "$ACTUAL" ]; then
                if [ "$ACTUAL" != "$EXPECTED" ]; then
                    err "checksum mismatch: expected $EXPECTED, got $ACTUAL"
                fi
                say "Checksum verified."
            fi
        fi
    fi

    chmod +x "$TMPDIR/clat"

    # Choose install location
    INSTALL_DIR="/usr/local/bin"
    NEED_SUDO=false

    if [ -w "$INSTALL_DIR" ]; then
        NEED_SUDO=false
    elif command -v sudo >/dev/null 2>&1; then
        NEED_SUDO=true
    else
        # Fall back to ~/.local/bin
        INSTALL_DIR="$HOME/.local/bin"
        mkdir -p "$INSTALL_DIR"
    fi

    say "Installing to $INSTALL_DIR/clat..."
    if [ "$NEED_SUDO" = "true" ]; then
        sudo install -m 755 "$TMPDIR/clat" "$INSTALL_DIR/clat"
    else
        install -m 755 "$TMPDIR/clat" "$INSTALL_DIR/clat"
    fi

    # Verify installation
    if command -v clat >/dev/null 2>&1; then
        echo ""
        bold "Lattice $VERSION installed successfully!"
        say "Run 'clat' to start the REPL."
    else
        echo ""
        bold "Lattice $VERSION installed to $INSTALL_DIR/clat"
        if echo "$PATH" | tr ':' '\n' | grep -qx "$INSTALL_DIR"; then
            say "Run 'clat' to start the REPL."
        else
            say "Add $INSTALL_DIR to your PATH:"
            say "  export PATH=\"$INSTALL_DIR:\$PATH\""
        fi
    fi
}

main "$@"
