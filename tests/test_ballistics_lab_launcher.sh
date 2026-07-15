#!/bin/sh
# Hermetic contract tests for scripts/ballistics-lab.sh.
#
# The launcher is copied into disposable bundles so repository-local discovery,
# relocation, working-directory behavior, and paths containing spaces are all
# exercised without invoking a real Lattice or ballistics binary.
set -eu

fail() {
    printf 'FAIL: %s\n' "$*" >&2
    exit 1
}

assert_equal() {
    expected=$1
    actual=$2
    label=$3
    if [ "$actual" != "$expected" ]; then
        printf 'FAIL: %s\n  expected: <%s>\n  actual:   <%s>\n' \
            "$label" "$expected" "$actual" >&2
        exit 1
    fi
}

assert_contains() {
    file=$1
    fragment=$2
    label=$3
    if ! grep -F -e "$fragment" "$file" >/dev/null 2>&1; then
        printf 'FAIL: %s\n  missing: <%s>\n  output:\n' "$label" "$fragment" >&2
        sed 's/^/    /' "$file" >&2
        exit 1
    fi
}

assert_not_invoked() {
    label=$1
    if [ -e "$CAPTURE/argv" ]; then
        printf 'FAIL: %s unexpectedly invoked clat\n' "$label" >&2
        sed 's/^/    /' "$CAPTURE/argv" >&2
        exit 1
    fi
}

SCRIPT_DIR=$(CDPATH='' cd -P "$(dirname "$0")" && pwd -P)
REPOSITORY_ROOT=$(CDPATH='' cd -P "$SCRIPT_DIR/.." && pwd -P)
SOURCE_LAUNCHER=${BALLISTICS_LAUNCHER_UNDER_TEST:-"$REPOSITORY_ROOT/scripts/ballistics-lab.sh"}
ORIGINAL_PATH=$PATH

if [ ! -f "$SOURCE_LAUNCHER" ]; then
    fail "launcher not found: $SOURCE_LAUNCHER"
fi
sh -n "$SOURCE_LAUNCHER"

TMP_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/lattice-ballistics-launcher.XXXXXX") || \
    fail 'could not create temporary directory'
TMP_ROOT=$(CDPATH='' cd -P "$TMP_ROOT" && pwd -P)

cleanup() {
    rm -rf "$TMP_ROOT"
}
trap cleanup 0 HUP INT TERM

make_fake_clat() {
    target=$1
    mkdir -p "${target%/*}"
    cat >"$target" <<'FAKE_CLAT'
#!/bin/sh
set -eu
: "${BALLISTICS_LAUNCHER_CAPTURE:?missing BALLISTICS_LAUNCHER_CAPTURE}"
printf '%s\n' "$0" >"$BALLISTICS_LAUNCHER_CAPTURE/program"
pwd -P >"$BALLISTICS_LAUNCHER_CAPTURE/cwd"
: >"$BALLISTICS_LAUNCHER_CAPTURE/argv"
for argument do
    printf '%s\n' "$argument" >>"$BALLISTICS_LAUNCHER_CAPTURE/argv"
done
exit "${FAKE_CLAT_EXIT:-0}"
FAKE_CLAT
    chmod +x "$target"
}

make_fake_engine() {
    target=$1
    mkdir -p "${target%/*}"
    cat >"$target" <<'FAKE_ENGINE'
#!/bin/sh
if [ "${1:-}" = solve-json ] && [ "${2:-}" = --help ]; then
    printf '%s\n' \
        'Solve one explicit-SI v1 JSON request from stdin' \
        'Usage: ballistics solve-json [OPTIONS]'
    exit 0
fi
exit 64
FAKE_ENGINE
    chmod +x "$target"
}

make_incompatible_engine() {
    target=$1
    mkdir -p "${target%/*}"
    cat >"$target" <<'FAKE_OLD_ENGINE'
#!/bin/sh
# Deliberately ignores argv and succeeds, like /usr/bin/true. A status-only
# capability probe would incorrectly accept this unrelated executable.
exit 0
FAKE_OLD_ENGINE
    chmod +x "$target"
}

setup_bundle() {
    case_name=$1
    with_local_clat=$2

    BUNDLE="$TMP_ROOT/$case_name bundle"
    mkdir -p "$BUNDLE/scripts" "$BUNDLE/examples/ballistics_lab"
    BUNDLE=$(CDPATH='' cd -P "$BUNDLE" && pwd -P)
    LAUNCHER="$BUNDLE/scripts/ballistics-lab.sh"
    cp "$SOURCE_LAUNCHER" "$LAUNCHER"
    chmod +x "$LAUNCHER"
    : >"$BUNDLE/examples/ballistics_lab/ballistics-repl.lat"

    if [ "$with_local_clat" = yes ]; then
        make_fake_clat "$BUNDLE/clat"
    fi

    WORK="$TMP_ROOT/$case_name working directory"
    CAPTURE="$TMP_ROOT/$case_name capture"
    mkdir -p "$WORK" "$CAPTURE"
    WORK=$(CDPATH='' cd -P "$WORK" && pwd -P)
    CAPTURE=$(CDPATH='' cd -P "$CAPTURE" && pwd -P)
}

clean_environment() {
    unset CLAT BALLISTICS_ENGINE BALLISTICS_LAB_BACKEND 2>/dev/null || :
    PATH=$ORIGINAL_PATH
    FAKE_CLAT_EXIT=0
    export PATH FAKE_CLAT_EXIT
}

run_launcher() {
    rm -f "$CAPTURE/program" "$CAPTURE/cwd" "$CAPTURE/argv" \
        "$CAPTURE/stdout" "$CAPTURE/stderr"
    set +e
    (
        cd "$WORK"
        BALLISTICS_LAUNCHER_CAPTURE=$CAPTURE
        export BALLISTICS_LAUNCHER_CAPTURE
        "$LAUNCHER" "$@"
    ) >"$CAPTURE/stdout" 2>"$CAPTURE/stderr"
    RUN_STATUS=$?
    set -e
}

line_at() {
    sed -n "${1}p" "$2"
}

assert_invocation() {
    expected_program=$1
    expected_backend_flag=$2
    expected_engine=$3
    label=$4

    [ -f "$CAPTURE/argv" ] || fail "$label did not invoke clat"
    assert_equal "$expected_program" "$(cat "$CAPTURE/program")" "$label clat executable"
    assert_equal "$BUNDLE" "$(cat "$CAPTURE/cwd")" "$label child working directory"

    expected_count=2
    engine_index=2
    if [ -n "$expected_backend_flag" ]; then
        expected_count=3
        engine_index=3
        assert_equal "$expected_backend_flag" "$(line_at 1 "$CAPTURE/argv")" \
            "$label backend flag"
        app_index=2
    else
        app_index=1
    fi

    actual_count=$(wc -l <"$CAPTURE/argv" | tr -d '[:space:]')
    assert_equal "$expected_count" "$actual_count" "$label argument count"

    app_argument=$(line_at "$app_index" "$CAPTURE/argv")
    case "$app_argument" in
        examples/ballistics_lab/ballistics-repl.lat | \
        "$BUNDLE/examples/ballistics_lab/ballistics-repl.lat")
            ;;
        *)
            fail "$label passed unexpected application path: $app_argument"
            ;;
    esac
    assert_equal "$expected_engine" "$(line_at "$engine_index" "$CAPTURE/argv")" \
        "$label engine executable"
}

PASSED=0
pass() {
    PASSED=$((PASSED + 1))
    printf '  ok: %s\n' "$1"
}

printf 'Running POSIX ballistics Lab launcher tests...\n'

# Repository-local clat wins over PATH, while the engine is discovered on PATH.
setup_bundle default yes
clean_environment
PATH_TOOLS="$TMP_ROOT/default PATH tools"
make_fake_clat "$PATH_TOOLS/clat"
make_fake_engine "$PATH_TOOLS/ballistics"
PATH="$PATH_TOOLS:/usr/bin:/bin"
export PATH
run_launcher
assert_equal 0 "$RUN_STATUS" 'default launch status'
assert_invocation "$BUNDLE/clat" '' "$PATH_TOOLS/ballistics" 'default launch'
pass 'default stack-vm and repository-local discovery'

# Command-line paths and backend selection override environment values. Every
# path deliberately contains spaces to catch accidental word splitting.
setup_bundle cli-precedence yes
clean_environment
ENV_TOOLS="$TMP_ROOT/environment tools"
CLI_TOOLS="$TMP_ROOT/command line tools"
PATH_TOOLS="$TMP_ROOT/precedence PATH tools"
make_fake_clat "$ENV_TOOLS/clat"
make_fake_engine "$ENV_TOOLS/ballistics"
make_fake_clat "$CLI_TOOLS/clat selected"
make_fake_engine "$CLI_TOOLS/ballistics selected"
make_fake_clat "$PATH_TOOLS/clat"
make_fake_engine "$PATH_TOOLS/ballistics"
CLAT="$ENV_TOOLS/clat"
BALLISTICS_ENGINE="$ENV_TOOLS/ballistics"
BALLISTICS_LAB_BACKEND=stack-vm
PATH="$PATH_TOOLS:/usr/bin:/bin"
export CLAT BALLISTICS_ENGINE BALLISTICS_LAB_BACKEND PATH
run_launcher --clat "$CLI_TOOLS/clat selected" \
    --engine "$CLI_TOOLS/ballistics selected" --backend regvm
assert_equal 0 "$RUN_STATUS" 'CLI precedence status'
assert_invocation "$CLI_TOOLS/clat selected" '--regvm' \
    "$CLI_TOOLS/ballistics selected" 'CLI precedence'
pass 'CLI discovery and backend precedence with spaced paths'

# Environment paths and backend win over repository-local/PATH fallbacks.
setup_bundle environment-precedence yes
clean_environment
ENV_TOOLS="$TMP_ROOT/environment precedence tools"
PATH_TOOLS="$TMP_ROOT/environment fallback tools"
make_fake_clat "$ENV_TOOLS/clat"
make_fake_engine "$ENV_TOOLS/ballistics"
make_fake_clat "$PATH_TOOLS/clat"
make_fake_engine "$PATH_TOOLS/ballistics"
CLAT="$ENV_TOOLS/clat"
BALLISTICS_ENGINE="$ENV_TOOLS/ballistics"
BALLISTICS_LAB_BACKEND=tree-walk
PATH="$PATH_TOOLS:/usr/bin:/bin"
export CLAT BALLISTICS_ENGINE BALLISTICS_LAB_BACKEND PATH
run_launcher
assert_equal 0 "$RUN_STATUS" 'environment precedence status'
assert_invocation "$ENV_TOOLS/clat" '--tree-walk' "$ENV_TOOLS/ballistics" \
    'environment precedence'
pass 'environment discovery and backend precedence'

# Both the long backend option and all shorthand spellings map to the exact
# clat argv contract. StackVM intentionally contributes no empty flag.
setup_bundle backend-forms no
clean_environment
BACKEND_TOOLS="$TMP_ROOT/backend form tools"
make_fake_clat "$BACKEND_TOOLS/clat"
make_fake_engine "$BACKEND_TOOLS/ballistics"

run_launcher --clat "$BACKEND_TOOLS/clat" --engine "$BACKEND_TOOLS/ballistics" \
    --backend stack-vm
assert_equal 0 "$RUN_STATUS" '--backend stack-vm status'
assert_invocation "$BACKEND_TOOLS/clat" '' "$BACKEND_TOOLS/ballistics" \
    '--backend stack-vm'

run_launcher --clat "$BACKEND_TOOLS/clat" --engine "$BACKEND_TOOLS/ballistics" \
    --backend regvm
assert_equal 0 "$RUN_STATUS" '--backend regvm status'
assert_invocation "$BACKEND_TOOLS/clat" '--regvm' "$BACKEND_TOOLS/ballistics" \
    '--backend regvm'

run_launcher --clat "$BACKEND_TOOLS/clat" --engine "$BACKEND_TOOLS/ballistics" \
    --backend tree-walk
assert_equal 0 "$RUN_STATUS" '--backend tree-walk status'
assert_invocation "$BACKEND_TOOLS/clat" '--tree-walk' "$BACKEND_TOOLS/ballistics" \
    '--backend tree-walk'

run_launcher --clat "$BACKEND_TOOLS/clat" --engine "$BACKEND_TOOLS/ballistics" --stack-vm
assert_equal 0 "$RUN_STATUS" '--stack-vm status'
assert_invocation "$BACKEND_TOOLS/clat" '' "$BACKEND_TOOLS/ballistics" '--stack-vm'

run_launcher --clat "$BACKEND_TOOLS/clat" --engine "$BACKEND_TOOLS/ballistics" --regvm
assert_equal 0 "$RUN_STATUS" '--regvm status'
assert_invocation "$BACKEND_TOOLS/clat" '--regvm' "$BACKEND_TOOLS/ballistics" '--regvm'

run_launcher --clat "$BACKEND_TOOLS/clat" --engine "$BACKEND_TOOLS/ballistics" --tree-walk
assert_equal 0 "$RUN_STATUS" '--tree-walk status'
assert_invocation "$BACKEND_TOOLS/clat" '--tree-walk' "$BACKEND_TOOLS/ballistics" '--tree-walk'
pass 'long and shorthand backend selection'

# command -v may return a relative path when PATH itself is relative. Both
# tools must be made absolute before the launcher changes to the bundle root.
setup_bundle relative-path no
clean_environment
RELATIVE_TOOLS="$WORK/relative tools"
make_fake_clat "$RELATIVE_TOOLS/clat"
make_fake_engine "$RELATIVE_TOOLS/ballistics"
PATH='relative tools:/usr/bin:/bin'
export PATH
run_launcher
assert_equal 0 "$RUN_STATUS" 'relative PATH status'
assert_invocation "$RELATIVE_TOOLS/clat" '' "$RELATIVE_TOOLS/ballistics" \
    'relative PATH resolution'
pass 'relative PATH entries survive the bundle-root chdir'

# Help and malformed options are parser-only operations and must not discover
# or launch either executable.
setup_bundle option-errors no
clean_environment
run_launcher --help
assert_equal 0 "$RUN_STATUS" '--help status'
assert_contains "$CAPTURE/stdout" '--backend' '--help output'
assert_not_invoked '--help'

run_launcher --backend unsupported
assert_equal 2 "$RUN_STATUS" 'invalid backend status'
assert_not_invoked 'invalid backend'

run_launcher --backend
assert_equal 2 "$RUN_STATUS" 'missing backend value status'
assert_not_invoked 'missing backend value'

run_launcher --unknown-option
assert_equal 2 "$RUN_STATUS" 'unknown option status'
assert_not_invoked 'unknown option'
pass 'help and option errors do not launch clat'

# An invalid explicit override is an error, not permission to silently select a
# lower-precedence repository-local or PATH executable.
setup_bundle invalid-overrides yes
clean_environment
FALLBACK_TOOLS="$TMP_ROOT/invalid override fallback tools"
make_fake_clat "$FALLBACK_TOOLS/clat"
make_fake_engine "$FALLBACK_TOOLS/ballistics"
PATH="$FALLBACK_TOOLS:/usr/bin:/bin"
export PATH
run_launcher --clat "$TMP_ROOT/missing clat" --engine "$FALLBACK_TOOLS/ballistics"
if [ "$RUN_STATUS" -eq 0 ]; then
    fail 'missing explicit clat unexpectedly succeeded'
fi
assert_contains "$CAPTURE/stderr" 'clat' 'missing explicit clat diagnostic'
assert_not_invoked 'missing explicit clat'

run_launcher --clat "$FALLBACK_TOOLS/clat" --engine "$TMP_ROOT/missing ballistics"
if [ "$RUN_STATUS" -eq 0 ]; then
    fail 'missing explicit engine unexpectedly succeeded'
fi
assert_contains "$CAPTURE/stderr" 'ballistics' 'missing explicit engine diagnostic'
assert_not_invoked 'missing explicit engine'
pass 'invalid explicit overrides fail without fallback'

# A stale executable named ballistics on PATH must not shadow a compatible
# sibling build. Explicitly selecting that same stale binary remains an error.
setup_bundle incompatible-engine yes
clean_environment
OLD_TOOLS="$TMP_ROOT/incompatible engine PATH tools"
SIBLING_ENGINE="$TMP_ROOT/ballistics-engine/target/release/ballistics"
make_incompatible_engine "$OLD_TOOLS/ballistics"
make_fake_engine "$SIBLING_ENGINE"
PATH="$OLD_TOOLS:/usr/bin:/bin"
export PATH
run_launcher
assert_equal 0 "$RUN_STATUS" 'incompatible PATH engine fallback status'
assert_invocation "$BUNDLE/clat" '' "$SIBLING_ENGINE" \
    'incompatible PATH engine fallback'
assert_contains "$CAPTURE/stderr" 'ignoring incompatible engine' \
    'incompatible PATH engine warning'

run_launcher --engine "$OLD_TOOLS/ballistics"
assert_equal 2 "$RUN_STATUS" 'explicit incompatible engine status'
assert_contains "$CAPTURE/stderr" 'required solve-json v1 command' \
    'explicit incompatible engine diagnostic'
assert_not_invoked 'explicit incompatible engine'
pass 'incompatible PATH engines are skipped but explicit engines fail'

# The launcher must replace itself with clat rather than swallowing its status.
setup_bundle exit-status no
clean_environment
EXIT_TOOLS="$TMP_ROOT/exit status tools"
make_fake_clat "$EXIT_TOOLS/clat"
make_fake_engine "$EXIT_TOOLS/ballistics"
FAKE_CLAT_EXIT=37
export FAKE_CLAT_EXIT
run_launcher --clat "$EXIT_TOOLS/clat" --engine "$EXIT_TOOLS/ballistics" --regvm
assert_equal 37 "$RUN_STATUS" 'clat exit propagation status'
assert_invocation "$EXIT_TOOLS/clat" '--regvm' "$EXIT_TOOLS/ballistics" \
    'clat exit propagation'
pass 'clat exit status is preserved'

printf 'Results: %s launcher test groups passed\n' "$PASSED"
