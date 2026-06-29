# Lattice Handbook Update Brief: v0.4.1 → v0.6.1

**Purpose:** This document captures every user-visible language change shipped in Lattice v0.5.0 (2026-06-11), v0.6.0 (2026-06-12), and v0.6.1 (2026-06-12), with enough context to update the Lattice Handbook without access to the implementation history. It is written for a documentation-updating model: each section states what changed, the before/after behavior, exact error message text, code examples that are verified to behave as shown, and which handbook topics are affected. Sections marked **[INTERNAL — do not document]** are context only.

**Versions covered:**

| Version | Date | One-line summary |
|---|---|---|
| v0.5.0 | 2026-06-11 | `freeze()` is now enforced on every backend; channel send rules unified; send-to-closed-channel errors |
| v0.6.0 | 2026-06-12 | Crystal sharing: frozen containers are shared by reference instead of deep-copied (all backends) |
| v0.6.1 | 2026-06-12 | Bug fixes only: register VM caught-error fix (Windows), LSP shutdown lifecycle. No semantic changes to document beyond a changelog entry |

**Background the handbook already covers (unchanged):** Lattice's phase system — `flux` (mutable/fluid), `fix` (immutable/crystal), `let` (inferred); `freeze()`, `thaw()`, `clone()`; `freeze(x) except ["k"]` partial freezing; `sublimate()` shallow freeze; structured concurrency with `scope`/`spawn`/channels/`select`; three execution backends (tree-walking interpreter, stack bytecode VM = default, register VM) that share one language semantics.

---

## 1. freeze() is now enforced everywhere (v0.5.0) — BREAKING

**What changed:** Before v0.5.0, mutating a frozen value silently succeeded in many cases (which cases depended on the backend). As of v0.5.0, every mutation path checks the phase system on every backend. This is the headline breaking change of v0.5.0: programs that mutated frozen values and got away with it now get runtime errors.

### 1.1 Index and field assignment on frozen containers

All of the following now raise runtime errors on every backend (previously: silent mutation on the default backend and/or tree-walker):

```lattice
flux a = [1, 2, 3]
freeze(a)
a[0] = 99            // ERROR: cannot modify a frozen value

flux m = Map::new()
m["k"] = 1
freeze(m)
m["k"] = 2           // ERROR: cannot modify a frozen value
m.k = 2              // ERROR (dot-assign on maps, same guard)

flux b = Buffer::new(4)
freeze(b)
b[0] = 255           // ERROR: cannot modify a frozen value
```

This applies identically to local bindings, global bindings, and nested containers (`a[0][1] = v` on a frozen nested array errors).

### 1.2 Mutating builtin methods reject frozen receivers

Every receiver-mutating builtin method now refuses crystal (frozen) or sublimated receivers, on every backend. The full list of guarded methods:

- **Array:** `push`, `pop`, `insert`, `remove_at`
- **Map:** `set`, `remove`, `merge`
- **Set:** `add`, `remove`, `clear`
- **Buffer:** `push`, `push_u16`, `push_u32`, every `write_*` method (`write_u8`/`u16`/`u32`/`u64`/`i64`), `clear`, `fill`, `resize`

Error message format (when the receiver is a named variable):

```
cannot call mutating method 'push' on crystal value 'arr' (use thaw(arr) to make it mutable)
```

Generic form otherwise: `cannot call mutating method 'X' on a frozen value` (or `... on a sublimated value`).

Read-only methods (`len`, `read_*`, `get`, `keys`, `contains`, `slice`, `to_*`, etc.) are unaffected — reading frozen data always works.

### 1.3 freeze-except semantics tightened and made uniform

`freeze(m) except ["k"]` (partial freeze with exempt keys/fields) behavior is now identical on all backends:

- After `freeze(m) except ["k"]`, the map itself reports phase **crystal** (`phase_of(m)` returns `"crystal"`; previously some backends reported `"fluid"`). The exempt keys remain writable.
- Exemption permits **value assignment only** (`m["k"] = v`, `m.k = v`, including compound forms `m["k"] += 1`). Method-based mutation is still blocked even for exempt keys: `m.set("k", v)` and `m.remove("k")` error on a freeze-except map.
- **A later full `freeze(m)` closes all exemptions.** Previously the exemption survived refreezing (a bug). Example:

```lattice
flux m = Map::new()
m["host"] = "h"
m["retries"] = 0
freeze(m) except ["retries"]
m["retries"] = 5      // OK — exempt
freeze(m)             // full refreeze
m["retries"] = 6      // ERROR — exemption closed
```

- **New keys are not exempt:** writing a key that did not exist at freeze time errors (`freeze(m) except ["host"]` then `m["fresh"] = 1` → error). Previously this was allowed on the tree-walker.
- Calling `freeze(m) except [...]` on an **already fully-frozen** map is accepted but punches no new holes — subsequent writes to the named keys still error.
- A key missing from the exemption table inherits the container's phase (frozen if the container is frozen). Previously the register VM treated missing keys as exempt.
- Error for an explicitly frozen key on an otherwise-writable map: `cannot modify frozen key 'k'`.

### 1.4 Handbook actions for section 1

- The chapter that introduces `freeze()` should state plainly that immutability is **runtime-enforced on every backend** for index/field assignment and mutating methods, with the error messages above.
- The freeze-except section needs the four rules from 1.3 (crystal parent phase, value-assignment-only exemption, refreeze closes holes, new keys not exempt).
- Add a migration note: code that relied on silently mutating frozen values breaks at v0.5.0 by design.

---

## 2. Channel send rules unified (v0.5.0) — BREAKING for some programs

**What changed:** The three backends previously enforced three different rules for what `.send()` accepts. There is now one rule, and sending to a closed channel is an error.

### 2.1 The send eligibility rule

A value may be sent over a channel if and only if **all** of:

1. It is not sublimated.
2. It is not fluid — **except** scalars (`Int`, `Float`, `Bool`, `Unit`), which are always sendable regardless of phase.
3. Its object graph contains **no closure, no Ref, and no iterator, at any depth, regardless of phase** (even frozen). These kinds hold internal state that cannot be safely transferred between threads. Channels *inside* values are fine.

Practical consequences (with before/after):

```lattice
let ch = Channel::new()

flux x = 7
ch.send(x)                 // OK now (fluid scalar) — used to error on the VMs

ch.send([1, 2, 3])         // OK now (unphased plain data) — used to error on the tree-walker

flux arr = [1, 2, 3]
ch.send(arr)               // ERROR (fluid container) — unchanged
ch.send(freeze(arr))       // OK — the canonical pattern, unchanged

let f = freeze(|x| { x })
ch.send(f)                 // ERROR now: cannot send a value containing a closure on a channel
                           // (previously accepted on EVERY backend — this was unsound)

let r = Ref::new(42)
ch.send(r)                 // ERROR: cannot send a value containing a Ref on a channel
```

Exact error messages:

- `can only send crystal (frozen) values on a channel` (fluid non-scalar)
- `cannot send a sublimated value on a channel`
- `cannot send a value containing a Ref on a channel` (similarly `...a closure...`, `...an iterator...`)

### 2.2 Send to a closed channel errors

```lattice
let ch = Channel::new()
ch.close()
ch.send(freeze(1))         // ERROR: cannot send on a closed channel
```

Previously this silently dropped the value on the default backend and register VM. `recv()` on a closed empty channel still returns `Unit` (unchanged).

### 2.3 Handbook actions for section 2

- Replace any statement like "only crystal (frozen) values can be sent" with the three-part rule above.
- The channel method table entry for `.send(val)` should read: sends a value (crystal, unphased, or scalar; values containing closures/Refs/iterators are rejected; errors if the channel is closed).
- Keep `ch.send(freeze(x))` as the canonical example — it remains correct and is now also the fast path (see section 3).

---

## 3. Crystal sharing (v0.6.0) — the major feature

**What changed:** Freezing a container now stores it **once**, and every subsequent copy shares it by reference with reference counting. Assigning, passing to functions, returning, sending over channels, and spawning threads over frozen data are all O(1) regardless of size. Before v0.6.0, Lattice deep-copied in all of those situations.

**Crucial framing for the handbook: semantics did not change.** Lattice is still strictly pass-by-value from the program's point of view; sharing is an optimization made safe by the v0.5.0 enforcement work (a frozen value provably cannot be mutated, so aliasing it is unobservable). A differential test mode that disables all sharing produces bit-identical program output, and this is enforced in CI.

### 3.1 What shares and what does not

Two kinds of frozen value exist internally:

- **Shared:** frozen arrays, maps, structs, sets, tuples, buffers, enums, and strings ≥ 32 bytes — stored once in shared immutable storage.
- **Legacy (plain copy semantics):** values containing closures, Refs, iterators, or channels anywhere in their graph; values with sublimated members; scalars; strings under 32 bytes. These freeze exactly as before (an immutability tag, ordinary copying).

A program **cannot observe** which kind it has — behavior is identical; only performance differs.

### 3.2 The operation table (suitable for the handbook nearly verbatim)

| Operation | Behavior since v0.6.0 |
|---|---|
| `freeze(container)` | One-time copy into shared storage — measured ≈1.9× the cost of a plain pre-v0.6.0 freeze, paid once |
| copy / pass / return / `send` / `spawn` over frozen data | O(1) share (reference count) |
| `thaw(v)` | Always a fresh mutable copy — other aliases of the frozen value are unaffected |
| `clone(v)` | Always a guaranteed physical copy — use it to detach a small piece, since keeping one shared element alive otherwise keeps its whole frozen structure alive |

The `clone()` note matters and is new guidance: extracting one element from a large shared frozen structure yields a handle that keeps the entire structure's memory alive; `clone(elem)` cuts the tie.

### 3.3 Concurrency interaction

- `ch.send(frozen_value)` is now a reference-count increment plus a tiny handle enqueue — previously two full deep copies. Receivers on other threads read the same immutable storage safely.
- `spawn` blocks that read frozen data from the enclosing scope share it instead of deep-copying the environment.
- All eligibility rules from section 2 are checked first; sharing replaces the copy only for values that were already legal to send.

```lattice
fix dataset = freeze(build_huge_array())
scope {
    spawn { consume(dataset) }   // shares the frozen data — no copy
    spawn { consume(dataset) }
}
```

### 3.4 Switches and debugging modes (document all three)

- **`clat --no-regions`** (CLI flag): disables sharing wholesale; every freeze behaves like a plain pre-v0.6.0 tag flip. Inherited by spawned threads.
- **`LATTICE_SHARE_CRYSTALS=0`** (environment variable, v0.6.0): runtime kill switch with the same effect as `--no-regions`; works for embedded hosts and the `clat-run` thin runtime where there is no CLI flag. Any value other than exactly `0` leaves sharing on.
- **`LATTICE_FORCE_COPY=1`** (environment variable): debugging oracle — every would-be share becomes a deep copy. Program output must be identical with and without it; if it differs, that is a bug to report.

### 3.5 Measured performance (cite as "measured on Apple M3 Max"; full tables in `benchmarks/RESULTS.md`, regenerate with `make bench-cbr`)

- Aliasing a frozen 10k-element array 2000×: ≥1250× faster than copying on both VMs; 6.2× on the tree-walker.
- Channel streaming of frozen payloads: 9.2–14.5× faster.
- Passing a frozen map as a function argument 100k×: 4.8–11.4× faster (per backend).
- Plain reads of frozen data: unchanged (reads were never copies).
- The cost case: a loop that clones and freezes a fresh 10k array each iteration runs ≈1.9× slower than with sharing disabled — the one-time materialization cost. Freeze-heavy code that never aliases can use `--no-regions`.

### 3.6 Known limitation to document

Two `Ref` cells that mutually contain each other, thawed concurrently from different threads, can deadlock (per-cell locking; requires deliberately constructed mutual ref cycles). This is a documented limitation, not a planned behavior.

### 3.7 Handbook actions for section 3

- Add a "Crystal Sharing" section to the phase-system chapter (the README's section of the same name, added in v0.6.0, is a good source — it was reviewed for accuracy against the implementation).
- Update the concurrency chapter: channel sends and spawns over frozen data are O(1); the freeze-before-send pattern is now also the high-performance pattern.
- Add the three switches (3.4) to the CLI/environment reference.
- Update `freeze`/`thaw`/`clone` builtin reference entries: the generated docs (`docs.html`, from source doc comments) already carry the new wording — mirror it.

---

## 4. Self-hosted compiler: nested index assignment (v0.5.0)

**What changed:** The self-hosted compiler (`compiler/latc.lat`) previously rejected chained index assignment targets as parse errors. It now compiles them, matching the C compiler:

```lattice
g[i][j] = v          // nested index assignment, any depth
m["k"][0] = v        // array-in-map
g[i][j] += v         // all compound operators: += -= *= /= %=
a[i + 1][j * 2] = v  // computed indices
```

Note (both compilers, pre-existing and unchanged): index expressions in the *target* of a nested assignment may be evaluated more than once during the write-back, so side-effecting index expressions (function calls in the brackets) should be avoided in assignment targets.

**Handbook action:** if the handbook has a self-hosted-compiler appendix with a parity/limitations table, remove "nested index assignment" from the limitations. Remaining known gaps in BOTH compilers (unchanged): generics are parsed but not functional; no map/set/buffer literals.

---

## 5. Bug fixes worth a changelog entry (v0.5.0–v0.6.1)

These need at most a line each in a changelog/errata section; they do not change documented semantics, they make the documented semantics true.

- **Iterator callbacks no longer double-free heap-backed values** (tree-walker). `iter(arr).map(|x| { x })` over string arrays could crash the process on v0.4.1 and earlier. Fixed in v0.6.0's development cycle.
- **`.remove()` on a frozen map crashed** the tree-walker (double-free) instead of erroring. Now a clean phase error (section 1.2).
- **Exception handling no longer leaks**: values abandoned during `try`/`catch` unwinding are released on both VMs (previously leaked copies; would have pinned shared memory).
- **Register VM, native Windows only (v0.6.1):** a *caught* runtime error (`try`/`catch` or `?`) could corrupt memory in switch-dispatch builds — i.e., the Windows `clat.exe`/`clat-run.exe` running with `--regvm`. POSIX builds were never affected. Windows users should upgrade to v0.6.1.
- **clat-lsp (v0.6.1):** the language server now implements the LSP `shutdown`/`exit` lifecycle correctly (it previously exited immediately after the `shutdown` response, which could crash conforming editor clients' exit write).
- **Spawned threads** now run with 8 MB stacks (previously platform default, as low as 512 KB), preventing stack overflow in deeply recursive spawned code.
- **Windows builds** compile against current mingw-w64 runtimes (v0.6.1).

---

## 6. Known divergences / open issues (document only if the handbook has a "known issues" section)

- Calling a callable struct field with the wrong number of arguments errors on the tree-walker but is silently accepted by both VMs (tracked: LAT-451).
- `--stats` prints nothing on the default (stack VM) backend; it works under `--tree-walk` (LAT-455).
- `+=` on a frozen *string local* is not yet phase-guarded on the stack VM fast path (LAT-454); ordinary frozen-value mutation is guarded per section 1.
- Mutating a `Ref` cell from a spawned thread while the parent uses it can crash the tree-walker at teardown; both VMs are safe since v0.6.1's development cycle. Cross-thread Ref mutation is best avoided (LAT-460); note Refs cannot cross channels at all (section 2.1).
- `--tree-walk --gc-stress` (a debug mode) can crash on freeze-except/anneal patterns; pre-existing, unrelated to the new features (LAT-453).

---

## 7. New developer tooling (document if the handbook covers the repo/contributing)

- `make bench-cbr` — crystal-sharing benchmark suite; writes `benchmarks/RESULTS.md`.
- `make bench-freeze-gate` — freeze-cost regression gate used in CI.
- `make tsan` — ThreadSanitizer build/run of all three backend suites plus the concurrency matrices (note: on Linux kernels with high mmap ASLR entropy, run under `setarch $(uname -m) -R make tsan`).
- `make test-force-copy` — runs the full suite under the `LATTICE_FORCE_COPY=1` oracle on all backends.
- `./build/test_runner --filter <substring>` — run a subset of the C test suite.
- New cross-backend language-level test matrices: `tests/cbr_alias_matrix.lat`, `tests/cbr_concurrency_matrix.lat`.

---

## 8. [INTERNAL — do not document] Implementation context for the curious editor

For the editing model's understanding only; none of this belongs in the handbook:

- Sharing is implemented as sealed, atomically-refcounted, process-global "crystal regions"; the value's region-id field holds a low-bit-tagged pointer. Freezing a shareable container force-copies it into a region once ("materialization" — the 1.9× cost); cloning a shared handle is a refcount retain plus a 72-byte struct copy; the last release frees the region's pages in O(1).
- The 32-byte string threshold and the scalar exclusion exist because copying small things is cheaper than refcount traffic.
- The staged rollout (design doc `design/crystal-by-reference.md`, status section included) was: Stage 0 = the v0.5.0 enforcement work (a prerequisite — sharing is only safe because mutation of frozen data is provably rejected); Stages 1–4 wired the three backends; Stage 5 = TSAN/concurrency hardening; Stage 6 = benchmarks/docs.
- The version bumps were deliberate semver communication: v0.5.0 minor because enforcement breaks formerly-silently-wrong programs; v0.6.0 minor for the feature; v0.6.1 patch for pure fixes.
