#!/bin/sh
# Launch the self-hosted Ballistics Lab from a source checkout.
set -eu

PROGRAM="ballistics-lab"

usage() {
    cat <<'EOF'
Usage: ballistics-lab [OPTIONS]

Launch the self-hosted Lattice Ballistics Lab.

Options:
  --backend NAME   Select stack-vm (default) or regvm
  --stack-vm       Use the default stack VM
  --regvm          Use the register VM
  --clat PATH      Use this clat executable
  --engine PATH    Use this ballistics executable
  -h, --help       Show this help

Environment:
  BALLISTICS_LAB_BACKEND  Backend name; defaults to stack-vm
  CLAT                    clat executable name or path
  BALLISTICS_ENGINE       ballistics executable name or path
  BALLISTICS_ENGINE_VERSION
                          Optional exact engine version required by the lab

Discovery is deterministic. Explicit options override environment variables.
Otherwise the launcher checks the checkout, PATH, Cargo target directories,
and a sibling ballistics-engine checkout.
EOF
}

die() {
    printf '%s: %s\n' "$PROGRAM" "$*" >&2
    exit 2
}

# Print an absolute path for an executable file. This happens before changing
# directory so relative explicit paths and relative PATH entries remain valid.
absolute_executable() {
    candidate=$1
    [ -f "$candidate" ] && [ -x "$candidate" ] || return 1

    directory=${candidate%/*}
    filename=${candidate##*/}
    if [ "$directory" = "$candidate" ]; then
        directory=.
    elif [ -z "$directory" ]; then
        directory=/
    fi

    absolute_directory=$(CDPATH='' cd -P "$directory" 2>/dev/null && pwd -P) || return 1
    printf '%s/%s\n' "$absolute_directory" "$filename"
}

resolve_executable() {
    requested=$1
    [ -n "$requested" ] || return 1

    case "$requested" in
        */*) absolute_executable "$requested" ;;
        *)
            discovered=$(command -v "$requested" 2>/dev/null) || return 1
            absolute_executable "$discovered"
            ;;
    esac
}

engine_supports_v1() {
    engine_help=$(LC_ALL=C "$1" solve-json --help </dev/null 2>&1) || return 1
    case "$engine_help" in
        *solve-json*) ;;
        *) return 1 ;;
    esac
    case "$engine_help" in
        *"explicit-SI v1 JSON request"*) return 0 ;;
        *) return 1 ;;
    esac
}

compatible_engine_path() {
    compatible_candidate=$(absolute_executable "$1" 2>/dev/null) || return 1
    if engine_supports_v1 "$compatible_candidate"; then
        printf '%s\n' "$compatible_candidate"
        return 0
    fi
    printf '%s: ignoring incompatible engine %s (no solve-json v1 command)\n' \
        "$PROGRAM" "$compatible_candidate" >&2
    return 1
}

compatible_engine_command() {
    compatible_candidate=$(resolve_executable "$1" 2>/dev/null) || return 1
    if engine_supports_v1 "$compatible_candidate"; then
        printf '%s\n' "$compatible_candidate"
        return 0
    fi
    printf '%s: ignoring incompatible engine %s (no solve-json v1 command)\n' \
        "$PROGRAM" "$compatible_candidate" >&2
    return 1
}

set_backend() {
    requested_backend=$1
    if [ "$backend_selected" -eq 1 ] && [ "$backend" != "$requested_backend" ]; then
        die "conflicting backend selections: $backend and $requested_backend"
    fi
    backend=$requested_backend
    backend_selected=1
}

SCRIPT_DIR=$(CDPATH='' cd -P "$(dirname "$0")" 2>/dev/null && pwd -P) || \
    die "cannot resolve launcher directory"
ROOT_DIR=$(CDPATH='' cd -P "$SCRIPT_DIR/.." 2>/dev/null && pwd -P) || \
    die "cannot resolve Lattice checkout"
CALLER_DIR=$(pwd -P)
REPL_PATH="$ROOT_DIR/examples/ballistics_lab/ballistics-repl.lat"

backend=${BALLISTICS_LAB_BACKEND:-stack-vm}
backend_selected=0
clat_request=${CLAT:-}
engine_request=${BALLISTICS_ENGINE:-}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --backend)
            [ "$#" -ge 2 ] || die "--backend requires a value"
            set_backend "$2"
            shift 2
            ;;
        --backend=*)
            set_backend "${1#*=}"
            shift
            ;;
        --stack-vm)
            set_backend stack-vm
            shift
            ;;
        --regvm)
            set_backend regvm
            shift
            ;;
        --clat)
            [ "$#" -ge 2 ] || die "--clat requires a value"
            clat_request=$2
            shift 2
            ;;
        --clat=*)
            clat_request=${1#*=}
            shift
            ;;
        --engine)
            [ "$#" -ge 2 ] || die "--engine requires a value"
            engine_request=$2
            shift 2
            ;;
        --engine=*)
            engine_request=${1#*=}
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        --)
            shift
            [ "$#" -eq 0 ] || die "unexpected positional argument: $1"
            ;;
        -*)
            die "unknown option: $1"
            ;;
        *)
            die "unexpected positional argument: $1"
            ;;
    esac
done

case "$backend" in
    stack-vm|regvm) ;;
    *) die "unknown backend '$backend' (expected stack-vm or regvm)" ;;
esac

[ -f "$REPL_PATH" ] || die "REPL source not found at $REPL_PATH"

if [ -n "$clat_request" ]; then
    clat=$(resolve_executable "$clat_request") || \
        die "clat executable '$clat_request' was not found or is not executable"
elif [ -x "$ROOT_DIR/clat" ]; then
    clat=$(absolute_executable "$ROOT_DIR/clat")
else
    clat=$(resolve_executable clat) || \
        die "clat was not found; build the checkout with 'make' or set CLAT"
fi

if [ -n "$engine_request" ]; then
    engine=$(resolve_executable "$engine_request") || \
        die "ballistics executable '$engine_request' was not found or is not executable"
    engine_supports_v1 "$engine" || \
        die "ballistics executable '$engine' does not provide the required solve-json v1 command"
else
    engine=$(compatible_engine_command ballistics || true)

    if [ -z "$engine" ]; then
        cargo_target=${CARGO_TARGET_DIR:-}
        if [ -n "$cargo_target" ]; then
            for candidate in \
                "$cargo_target/release/ballistics" \
                "$cargo_target/debug/ballistics"; do
                engine=$(compatible_engine_path "$candidate" || true)
                [ -z "$engine" ] || break
            done
        fi
    fi

    if [ -z "$engine" ]; then
        for candidate in \
            "$ROOT_DIR/../ballistics-engine/target/release/ballistics" \
            "$ROOT_DIR/../ballistics-engine/target/debug/ballistics" \
            "$CALLER_DIR/target/release/ballistics" \
            "$CALLER_DIR/target/debug/ballistics" \
            "${HOME:-}/.cargo/bin/ballistics"; do
            engine=$(compatible_engine_path "$candidate" || true)
            [ -z "$engine" ] || break
        done
    fi

    [ -n "$engine" ] || die \
        "ballistics was not found; install it, build ../ballistics-engine, or set BALLISTICS_ENGINE"
fi

printf '%s: backend=%s, engine=%s\n' "$PROGRAM" "$backend" "$engine" >&2

# Lab imports are rooted at examples/ballistics_lab, so always execute from the
# bundle root regardless of the caller's working directory.
cd "$ROOT_DIR"
case "$backend" in
    stack-vm) exec "$clat" "$REPL_PATH" "$engine" ;;
    regvm) exec "$clat" --regvm "$REPL_PATH" "$engine" ;;
esac
