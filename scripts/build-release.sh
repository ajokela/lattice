#!/bin/bash
# Build release binaries for all platforms, create GitHub release, deploy to website.
# Usage: ./scripts/build-release.sh v0.3.27
#        ./scripts/build-release.sh --dry-run v0.3.27
set -euo pipefail

DRY_RUN=false
FORCE=false
VERSION=""
SITE_DIR="../lattice-lang.org"
BUILD_OUT="release-out"

# BSD VM SSH hosts (configured in ~/.ssh/config with ProxyJump through fileserver)
BSD_HOSTS=(
    freebsd-amd64
    freebsd-arm64
    openbsd-amd64
    openbsd-arm64
    netbsd-amd64
    netbsd-arm64
)

# ── Helpers ──────────────────────────────────────────────────────────────────

say()  { printf "\033[1;34m=>\033[0m %s\n" "$*"; }
err()  { printf "\033[1;31merror:\033[0m %s\n" "$*" >&2; exit 1; }
warn() { printf "\033[1;33mwarn:\033[0m %s\n" "$*"; }
ok()   { printf "\033[1;32mok:\033[0m %s\n" "$*"; }

run() {
    if [ "$DRY_RUN" = "true" ]; then
        echo "[dry-run] $*"
    else
        "$@"
    fi
}

# ── Parse args ───────────────────────────────────────────────────────────────

while [ $# -gt 0 ]; do
    case "$1" in
        --dry-run) DRY_RUN=true; shift ;;
        --force)   FORCE=true; shift ;;
        v*)        VERSION="$1"; shift ;;
        *)         err "unknown argument: $1" ;;
    esac
done

[ -z "$VERSION" ] && err "usage: $0 [--dry-run] v<VERSION>"

# Strip leading 'v' for comparison with lattice.h
VERSION_NUM="${VERSION#v}"

# ── Phase 1: Preflight ──────────────────────────────────────────────────────

say "Preflight checks for $VERSION"

# Check version matches lattice.h
HEADER_VERSION=$(grep 'LATTICE_VERSION' include/lattice.h | head -1 | sed 's/.*"\(.*\)".*/\1/')
if [ "$HEADER_VERSION" != "$VERSION_NUM" ]; then
    err "version mismatch: lattice.h has $HEADER_VERSION, expected $VERSION_NUM"
fi
ok "Version matches lattice.h"

# Check git tree is clean
if [ -n "$(git status --porcelain)" ]; then
    warn "Git tree is dirty — untracked/modified files exist"
    if [ "$DRY_RUN" = "false" ] && [ "$FORCE" = "false" ]; then
        read -rp "Continue anyway? [y/N] " REPLY
        [ "$REPLY" = "y" ] || exit 1
    fi
fi

# Check gh auth
if ! gh auth status >/dev/null 2>&1; then
    err "gh is not authenticated — run 'gh auth login'"
fi
ok "GitHub CLI authenticated"

# Check Docker
if ! docker info >/dev/null 2>&1; then
    err "Docker is not running"
fi
ok "Docker is running"

# Check SSH to fileserver
if ! ssh -o ConnectTimeout=5 alex@fileserver.localnet true 2>/dev/null; then
    warn "Cannot reach fileserver.localnet — BSD builds will be skipped"
    SKIP_BSD=true
else
    ok "SSH to fileserver.localnet"
    SKIP_BSD=false
fi

# Check SSH to RISC-V host
RISCV_HOST="root@10.1.1.26"
if ! ssh -o ConnectTimeout=5 -o BatchMode=yes "$RISCV_HOST" true 2>/dev/null; then
    warn "Cannot reach $RISCV_HOST — RISC-V build will be skipped"
    SKIP_RISCV=true
else
    ok "SSH to $RISCV_HOST (linux-riscv64)"
    SKIP_RISCV=false
fi

# ── Phase 2: Build ──────────────────────────────────────────────────────────

mkdir -p "$BUILD_OUT/$VERSION"
PIDS=()
FAILURES=()

build_bg() {
    local name="$1" cmd="$2"
    (
        say "Building $name..."
        if eval "$cmd"; then
            ok "$name"
        else
            warn "FAILED: $name"
            exit 1
        fi
    ) &
    PIDS+=($!)
}

# Create a source tarball for Docker builds (avoids sharing local build dir)
SRC_TAR=$(mktemp /tmp/lattice-src-XXXXXX.tar.gz)
git archive --format=tar.gz HEAD > "$SRC_TAR"
trap 'rm -f "$SRC_TAR"' EXIT

# Also add release-out to .gitignore awareness (it's a temp working dir)
say "Output directory: $BUILD_OUT/$VERSION"

# macOS builds run sequentially (they share the local filesystem)
say "Building macOS binaries..."

# macOS arm64 (native)
say "Building darwin-aarch64..."
if make release 2>&1 | tail -1; then
    mv clat-darwin-aarch64 "$BUILD_OUT/$VERSION/"
    ok "darwin-aarch64"
else
    warn "FAILED: darwin-aarch64"
    FAILURES+=("darwin-aarch64")
fi

say "Building clat-run darwin-aarch64..."
if make runtime-release 2>&1 | tail -1; then
    mv clat-run-darwin-aarch64 "$BUILD_OUT/$VERSION/"
    ok "clat-run darwin-aarch64"
else
    warn "FAILED: clat-run darwin-aarch64"
    FAILURES+=("clat-run-darwin-aarch64")
fi

# macOS x86_64 (Rosetta — may need TLS=0 if Homebrew is arm64-only)
say "Building darwin-x86_64..."
if arch -x86_64 make release 2>&1 | tail -1; then
    mv clat-darwin-x86_64 "$BUILD_OUT/$VERSION/"
    ok "darwin-x86_64"
else
    warn "Retrying darwin-x86_64 with TLS=0 (Homebrew libs may be arm64-only)..."
    if arch -x86_64 make release TLS=0 2>&1 | tail -1; then
        mv clat-darwin-x86_64 "$BUILD_OUT/$VERSION/"
        ok "darwin-x86_64 (no TLS)"
    else
        warn "FAILED: darwin-x86_64"
        FAILURES+=("darwin-x86_64")
    fi
fi

say "Building clat-run darwin-x86_64..."
if arch -x86_64 make runtime-release 2>&1 | tail -1; then
    mv clat-run-darwin-x86_64 "$BUILD_OUT/$VERSION/"
    ok "clat-run darwin-x86_64"
else
    warn "Retrying clat-run darwin-x86_64 with TLS=0..."
    if arch -x86_64 make runtime-release TLS=0 2>&1 | tail -1; then
        mv clat-run-darwin-x86_64 "$BUILD_OUT/$VERSION/"
        ok "clat-run darwin-x86_64 (no TLS)"
    else
        warn "FAILED: clat-run darwin-x86_64"
        FAILURES+=("clat-run-darwin-x86_64")
    fi
fi

# Windows build (cross-compile from macOS using MinGW)
say "Building windows-x86_64..."
if make clean && make WINDOWS=1 release 2>&1 | tail -1; then
    mv clat-windows-x86_64.exe "$BUILD_OUT/$VERSION/"
    ok "windows-x86_64"
else
    warn "FAILED: windows-x86_64"
    FAILURES+=("windows-x86_64")
fi

say "Building clat-run windows-x86_64..."
if make WINDOWS=1 runtime-release 2>&1 | tail -1; then
    mv clat-run-windows-x86_64.exe "$BUILD_OUT/$VERSION/"
    ok "clat-run windows-x86_64"
else
    warn "FAILED: clat-run windows-x86_64"
    FAILURES+=("clat-run-windows-x86_64")
fi

# Linux builds via Docker (parallel, using tarball copy to avoid shared state)
# Output dir is mounted so we can extract the binary without stdout corruption
ABSOUT="$(pwd)/$BUILD_OUT/$VERSION"

build_bg "linux-x86_64" "
    docker run --rm --platform linux/amd64 \
        -v '$SRC_TAR:/tmp/src.tar.gz:ro' \
        -v '$ABSOUT:/out' \
        alpine:3.19 sh -c '
            apk add --no-cache build-base libedit-static libedit-dev openssl-dev openssl-libs-static linux-headers &&
            mkdir /build && cd /build &&
            tar xzf /tmp/src.tar.gz &&
            make release STATIC=1 &&
            cp clat-linux-x86_64 /out/ &&
            make runtime-release STATIC=1 &&
            cp clat-run-linux-x86_64 /out/
        '
"

build_bg "linux-aarch64" "
    docker run --rm --platform linux/arm64 \
        -v '$SRC_TAR:/tmp/src.tar.gz:ro' \
        -v '$ABSOUT:/out' \
        alpine:3.19 sh -c '
            apk add --no-cache build-base libedit-static libedit-dev openssl-dev openssl-libs-static linux-headers &&
            mkdir /build && cd /build &&
            tar xzf /tmp/src.tar.gz &&
            make release STATIC=1 &&
            cp clat-linux-aarch64 /out/ &&
            make runtime-release STATIC=1 &&
            cp clat-run-linux-aarch64 /out/
        '
"

# RISC-V Linux build via SSH (has make, builds directly)
if [ "${SKIP_RISCV:-false}" = "false" ]; then
    build_bg "linux-riscv64" "
        ssh $RISCV_HOST 'rm -rf /tmp/lattice-build && mkdir -p /tmp/lattice-build' && \
        scp '$SRC_TAR' $RISCV_HOST:/tmp/lattice-build/src.tar.gz && \
        ssh $RISCV_HOST 'cd /tmp/lattice-build && tar xzf src.tar.gz && make release STATIC=1 && make runtime-release STATIC=1' && \
        scp $RISCV_HOST:/tmp/lattice-build/clat-linux-riscv64 $ABSOUT/clat-linux-riscv64 && \
        scp $RISCV_HOST:/tmp/lattice-build/clat-run-linux-riscv64 $ABSOUT/clat-run-linux-riscv64 && \
        ssh $RISCV_HOST 'rm -rf /tmp/lattice-build'
    "
fi

# BSD builds via SSH (parallel, each on its own VM)
# Uses build-bsd.sh — no gmake or git required on the VM, just cc + base system
if [ "${SKIP_BSD:-false}" = "false" ]; then
    for host in "${BSD_HOSTS[@]}"; do
        OS_PART="${host%-*}"
        ARCH_PART="${host##*-}"
        case "$ARCH_PART" in
            amd64) ARCH="x86_64"  ;;
            arm64) ARCH="aarch64" ;;
            *)     ARCH="$ARCH_PART" ;;
        esac
        BINARY="clat-${OS_PART}-${ARCH}"

        RUNTIME_BINARY="clat-run-${OS_PART}-${ARCH}"
        if ssh -o ConnectTimeout=5 -o BatchMode=yes "$host" true 2>/dev/null; then
            build_bg "$BINARY" "
                ssh $host 'rm -rf /tmp/lattice-build && mkdir -p /tmp/lattice-build' && \
                scp '$SRC_TAR' $host:/tmp/lattice-build/src.tar.gz && \
                scp scripts/build-bsd.sh $host:/tmp/lattice-build/build-bsd.sh && \
                ssh $host 'cd /tmp/lattice-build && tar xzf src.tar.gz && sh build-bsd.sh' && \
                scp $host:/tmp/lattice-build/$BINARY $ABSOUT/$BINARY && \
                scp $host:/tmp/lattice-build/$RUNTIME_BINARY $ABSOUT/$RUNTIME_BINARY && \
                ssh $host 'rm -rf /tmp/lattice-build'
            "
        else
            warn "Skipping $BINARY — cannot reach $host"
        fi
    done
fi

# Wait for all background builds (Docker + BSD)
say "Waiting for background builds to complete..."
BUILD_FAILED=false
for pid in "${PIDS[@]}"; do
    if ! wait "$pid"; then
        BUILD_FAILED=true
    fi
done

if [ "$BUILD_FAILED" = "true" ]; then
    warn "Some builds failed"
fi

# ── Phase 3: Checksums ──────────────────────────────────────────────────────

say "Generating checksums..."
cd "$BUILD_OUT/$VERSION"
BINARIES=$(ls clat-* 2>/dev/null || true)
if [ -z "$BINARIES" ]; then
    err "no binaries found in $BUILD_OUT/$VERSION"
fi

if command -v sha256sum >/dev/null 2>&1; then
    sha256sum clat-* > checksums.txt
else
    shasum -a 256 clat-* > checksums.txt
fi
cd - >/dev/null

ok "Checksums written to $BUILD_OUT/$VERSION/checksums.txt"
cat "$BUILD_OUT/$VERSION/checksums.txt"

# ── Phase 4: GitHub Release ─────────────────────────────────────────────────

say "Creating GitHub release $VERSION..."
NOTES="Lattice $VERSION

Binaries:
$(ls "$BUILD_OUT/$VERSION"/clat-* | while read -r f; do echo "- $(basename "$f")"; done)

Install: \`curl -fsSL https://lattice-lang.org/install.sh | sh\`"

run gh release create "$VERSION" \
    --title "Lattice $VERSION" \
    --notes "$NOTES" \
    "$BUILD_OUT/$VERSION"/clat-* \
    "$BUILD_OUT/$VERSION/checksums.txt"

ok "GitHub release created"

# ── Phase 5: Website deploy ─────────────────────────────────────────────────

if [ -d "$SITE_DIR" ]; then
    say "Deploying to website..."

    # Copy binaries
    run mkdir -p "$SITE_DIR/releases/$VERSION"
    run cp "$BUILD_OUT/$VERSION"/clat-* "$SITE_DIR/releases/$VERSION/"
    run cp "$BUILD_OUT/$VERSION/checksums.txt" "$SITE_DIR/releases/$VERSION/"

    # Copy install script to website root
    run cp scripts/install.sh "$SITE_DIR/install.sh"

    # Write latest.json
    DATE=$(date +%Y-%m-%d)
    run sh -c "echo '{\"version\": \"$VERSION\", \"date\": \"$DATE\"}' > '$SITE_DIR/releases/latest.json'"

    # Deploy
    if [ "$DRY_RUN" = "false" ]; then
        (cd "$SITE_DIR" && firebase deploy)
    else
        echo "[dry-run] firebase deploy in $SITE_DIR"
    fi

    ok "Website deployed"
else
    warn "Site directory $SITE_DIR not found — skipping website deploy"
fi

# ── Summary ──────────────────────────────────────────────────────────────────

echo ""
say "Release $VERSION complete!"
echo ""
echo "  Binaries:  $BUILD_OUT/$VERSION/"
echo "  GitHub:    https://github.com/alexjokela/lattice/releases/tag/$VERSION"
echo "  Install:   curl -fsSL https://lattice-lang.org/install.sh | sh"
echo ""

if [ ${#FAILURES[@]} -gt 0 ]; then
    warn "Failed builds: ${FAILURES[*]}"
    exit 1
fi
