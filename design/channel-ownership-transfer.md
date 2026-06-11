All load-bearing claims verified against the working tree. Writing the final design document.

---

# Lattice Channel Ownership Transfer — Final Design (LAT: "move")

**Status:** Approved for implementation. Supersedes the three candidate designs (explicit-move, sublimate-as-move, implicit-fluid-move) and resolves or explicitly descopes every judge-found hole.
**Chosen direction:** Explicit `move(x)`, valid only as the direct argument of a channel send, with a binding-level `VAL_MOVED` tombstone. Implicit fluid-move and sublimate-as-move are deliberately deferred (§9). Plain `ch.send(x)` behavior is **byte-for-byte unchanged in v1** on all three backends.
**Mechanism:** the probe-proven "disown-to-malloc" transfer (commit `24e72dd`, ASan/UBSan-clean, 1856 tests × 3 backends; 7.3× faster than detach on a 64k-entry map).

All file:line references below were re-verified against `/Users/alexjokela/projects/lattice` on 2026-06-10.

---

## 1. Why this surface

The judges ranked explicit-move highest (5.5) and falsified the other two angles on their own premises:

- **Implicit fluid-move** is dead: "fluid sends error today" is not dead semantic space in a language with `try/catch` (the error is catchable and recoverable on the default backend), `tests/test_stdlib.c:2662` (`test_channel_crystal_only_send`) pins the exact error it deletes, and move-vs-copy keyed to invisible phase provenance (`let y = m` carries `VTAG_FLUID`) is unteachable.
- **Sublimate-as-move** is dead for v1: its no-copy-of-sublimated rule is refuted by tests already in the suite (`phase_of(x)` and reaction callbacks copy sublimated values), the read-vs-copy distinction has no implementable boundary in a codebase where `env_get` (src/env.c:101) deep-clones every fetch, and recv-side "condensation" requires per-recv phase flips at 6 call sites plus a pre-sublimation phase history the single `VTAG` field cannot represent.
- **Explicit move** survives with repairable defects. It is opt-in (zero behavior change for existing programs), the moved-from state is unambiguous at the send site, and — decisively — **the value keeps its phase across the channel**: a fluid moves and arrives fluid, a crystal moves as whatever the crystal transfer mechanism is. No condensation, no recv-side changes at all (`channel_recv`, all select machinery, all recv call sites: untouched).

`move` joins the `freeze`/`thaw`/`sublimate` operator family as a full keyword (grep confirms zero `move` identifier uses in `tests/`, `examples/`, `lib/`, `compiler/latc.lat`, `framework/`). It is *not* a phase transition: phases describe values; after a move the binding holds no value. No reaction fires on move (a reaction touching the just-moved binding would itself be a use-after-move; firing nothing closes the cascade hazard by construction).

**The one-sentence model:** `ch.send(move(x))` transfers `x`'s value into the channel without freeze and (for plain data) without copy; `x` becomes an empty vessel that errors on any read and is revived by assignment. `ch.send(x)` is unchanged forever-backward-compatibly: crystal-only, deep copy.

### Position and target restrictions (v1, all enforced at compile time where a compiler exists, at eval time in the tree-walker)

1. `move(x)` is legal **only** as the sole argument of a `.send(...)` call. Anywhere else: "move is only valid as a channel send argument".
2. The operand must be a **bare identifier**. `move(arr[0])` / `move(s.f)` / `move(f())`: "move requires a variable; move the whole container, or extract first: `let v = arr.pop()`". Moving a container moves all nested fluid children and tombstones the one binding — sound because pass-by-value means the binding is the only root.
3. **Inside `scope`/`spawn`/`select` bodies, the operand must be declared inside that body.** Rationale: stackvm runs those bodies as sub-chunks against deep-cloned env exports (`stackvm_export_locals_to_env`, src/stackvm.c:821-834, called at :7684), so a move of an enclosing local would move the *clone* on the VMs but the *real binding* on the tree-walker — the cross-backend divergence the implicit-move judges flagged as a flagship failure. The restriction pins all three backends to one rule. The compilers enforce it structurally (inside sub-chunk compilation, an enclosing local resolves as a global; `move` on a global is rejected *in sub-chunk context only*); the tree-walker records the env scope depth at body entry and rejects moves of bindings defined shallower. v2 may lift this with synchronized export semantics.
4. Moving **through an upvalue** (a captured variable, `loc_type=1`) is allowed. The tombstone is written through the shared `ObjUpvalue` cell, so every VM closure sharing the capture errors cleanly on next read; tree-walker closures hold deep-cloned envs and keep their independent copies — the same observable divergence the backends already have for capture visibility (scalar assignment through captures). Documented and pinned with per-backend tests, exactly like the existing divergence.

### Eligibility matrix (what `move(x)` does, by value kind)

| Value | Behavior |
|---|---|
| Plain data (str, array, map, struct, set, tuple, buffer, enum) with all-`REGION_NONE` ownership | **Zero-copy move** (disown + raw push) |
| `VAL_CHANNEL` payload | Bitwise pointer handoff; refcount untouched (refcount is already atomic — `channel_retain`, src/channel.c:19) |
| Scalars (int/float/bool/unit/nil/range) | Bitwise move (no heap blocks); tombstone still written — semantics uniform |
| Graph containing `REGION_EPHEMERAL` or crystal-region (region_id ≠ REGION_NONE) nodes | **Whole-value detach-copy fallback** (`value_detach`, src/value.c:35), binding still tombstoned. Semantics uniform; zero-copy is the optimization. Optional `--warn-move-copy` diagnostic. This is the crystallized-layouts integration seam (§7). |
| Graph containing `VAL_REF`, `VAL_CLOSURE`, or `VAL_ITERATOR` anywhere | **Hard runtime error**, binding untouched and fully usable (collect-then-commit). |
| `VAL_MOVED` (double move) | Use-after-move error |

The hard-error row is the single most important correction adopted from the judges: `value_deep_clone` **shares** these kinds rather than copying them — `ref_retain` on the same cell with a **non-atomic** refcount (src/value.c:509, 285-295), shared `captured_env` for closures (src/value.c:361, 371), shared iterator state. The original design's "transparent detach fallback" would have shipped shared mutable non-atomically-refcounted state across the thread boundary — recreating the exact unsoundness move exists to prevent. v1 errors. (The pre-existing fact that UNPHASED refs can cross channels via plain send today is a separate bug; filed, not fixed here — §8.)

Known accepted limitation: `ch.send(move(ch))` parks a channel in its own buffer and leaks the cycle (buffered self-reference holds a refcount forever). This is reachable today with unphased channel sends on the VMs; documented, not solved by this feature.

---

## 2. The moved-from story: `VAL_MOVED`

A new `ValueType` member **appended after `VAL_ITERATOR`** (include/value.h:32 — appending preserves all serialized tag numbers), payload `as.moved = { const char *name; int line; }` where `name` is interned (process-global, mutex-protected intern table; borrowed, never freed — the same cross-thread-safe property `struct field_names[i]` already rely on across spawn).

It is a **type, not a phase tag**: every reader in the codebase dispatches on `type` first, and a hypothetical `VTAG_MOVED` array would still carry freed `elems` pointers — a UB trap. The tombstone owns zero heap pointers: `value_free` is a no-op, `value_clone_fast`/`value_deep_clone` are bitwise pass-through, `gc_mark_value` skips it, and it is never serialized to `.latc` constants (verifier rejects it in constant pools).

**Invariant: tombstones exist only as binding values** — stack slots, upvalue cells, globals, env entries. They are constructed solely by the send-move instruction and written solely through binding locations. Because every binding *read* is checked (§4), a tombstone can never be copied into a container, an argument, or a channel. This is what exempts container/serialization/builtin code from handling `VAL_MOVED` — and unlike the original design, the read-site coverage that upholds the invariant is complete on all three backends (§4), so the invariant actually holds.

**Revival:** assignment (`x = fresh`) and `let` overwrite the tombstone unconditionally (freeing it is a no-op); SET paths need no checks. A move in a loop works naturally when the binding is refilled each iteration; without refill, iteration 2 errors loudly.

**Compound assignment is a read:** `x += 1` on a moved binding errors (the read side of `resolve_lvalue`/`OP_INC_LOCAL` is checked), it does not revive.

**Introspection (new, per judges' debugging hole):** `is_moved(x)` — compiled like `move` (ident-only special form emitting a raw slot-type check that bypasses the moved-check) — returns a bool without erroring. This is the sanctioned probe; `try/catch` also works (all move errors are ordinary catchable runtime errors; post-catch the tombstone persists and the binding is revivable). The REPL prints `use of moved value 'x'` with provenance "sent on a channel at line N" in file mode and "sent earlier in this session" in REPL mode (REPL line numbers restart per entry — src/main.c lexes each input fresh — so the per-entry line is suppressed there). `chunk->local_names` may be absent in stripped `.latc` files (src/latc.c guards for it); the message falls back to the tombstone's own interned name, which is why the name lives in the payload rather than being looked up.

---

## 3. Runtime mechanics

### 3.1 The single fused instruction

`ch.send(move(x))` compiles to: `[receiver expr]` then **`OP_SEND_MOVE_VAR (name_idx, loc_type, loc_slot)`** — one variable-length opcode using the exact operand triple of `OP_FREEZE_VAR` (emission template src/stackcompiler.c:1783-1805; regvm targeting template `ROP_SUBLIMATE_VAR`, src/regvm.c:4980-5010). One opcode, strictly ordered, no observable intermediate state:

1. **Channel check first.** Peek the receiver operand. Not `VAL_CHANNEL` → runtime error `"move requires a channel send; '<type>' is not a channel"`, **binding untouched**. This resolves the dispatch-hijack hole: the original design's compile-time pattern match on the method *name* "send" committed to the take before the receiver's type was known (send is runtime dispatch — src/stackvm.c:2241 `case VAL_CHANNEL:` + `MHASH_send`), so a struct with a callable `send` field would have had its argument destroyed by a type error. Now the type error precedes the take. Corollary, documented: user-defined `send` methods cannot accept `move()` arguments — that is a feature (`move` means channel transfer), not a gap. The `PIC_CHANNEL_SEND` inline-cache path (include/inline_cache.h:265, classified at src/stackvm.c:1137 and src/regvm.c:837) is untouched: plain sends keep their PIC; `OP_SEND_MOVE_VAR` is its own opcode and never enters PIC dispatch, so cached and uncached paths cannot disagree.
2. **Moved check.** Binding already `VAL_MOVED` → use-after-move error (double move is a use).
3. **Classify** (`transfer_classify` in new `src/transfer.c`): one recursive walk. Ref/closure/iterator anywhere → error, **binding untouched** (collect-then-commit: nothing has been disowned). Ephemeral/region nodes anywhere → mark whole value for detach fallback.
4. **Take-and-blank.** Bitwise-lift the binding's `LatValue` into a C local and write the tombstone into the same location **in the same step** — a raw store, *not* `stackvm_write_back`. This resolves the verified double-free: `stackvm_write_back` (src/stackvm.c:534-550) `value_free`s the old slot contents and `value_deep_clone`s the new — calling it after a bitwise lift would free the blocks the receiver is about to own. The take uses the write-back's *location-resolution* logic (local slot / `frame->upvalues[i]->location` / global env entry) with lift-and-overwrite stores. From this point the slot is a pointer-free POD; no GC mark can reach the lifted blocks through the binding, and `transfer.c` uses **plain malloc/free for all scratch state** (hash set, work stack) so no `lat_alloc` — hence no GC trigger — occurs between take and push.
5. **Fallback path** (ephemeral/region): `value_detach` the lifted value, `value_free` the original (sender-thread, still registry-tracked — correct), then proceed to step 7 with the detached copy.
6. **Disown** (zero-copy path): `xfer_collect` gathers every owned `REGION_NONE` pointer (mirroring `value_free`'s `lat_free` sites: `str_val`; array `elems`, recursing `[0,len)` only — `[len,cap)` is uninitialized and the spare capacity rides along; struct `name` + `field_names` array + `field_values` + `field_phases` but never the interned `field_names[i]`; the `LatMap` struct only — hashmap entries/keys are already plain `calloc`/`strdup` in src/ds/hashmap.c; set/tuple/buffer/enum internals). Then `fluid_disown_set`: **one** O(sender-heap) pass over `FluidHeap.allocs`, unlinking and freeing only the tracking nodes ("remove if present", never assert `collected==disowned` — values built under `g_heap==NULL` are legitimately absent), and **decrementing the heap's live-count/byte accounting** so GC thresholds don't drift. The blocks are now plain-malloc-owned — bitwise identical ownership to a `value_detach` result. Measured: 3.66 ms vs 26.66 ms for a 64k-entry map; 0.13 ms floor for one small value in a 64k-entry heap. The O(heap) floor is the documented v1 perf cliff; the hash-indexed registry is a filed follow-up.
7. **Push under the channel mutex, closed-check first.** Lock `ch->mutex`; if `ch->closed`: unlock, **restore the value bitwise into the binding** (overwriting the tombstone — a POD, plain store), raise `"cannot send on a closed channel"`. **The value is never lost** — this replaces the original design's lossy free-and-stay-tombstoned rule, which the judges correctly compared unfavorably to Rust's `SendError`. The restored value is now heap-untracked (plain-malloc); that is sound and invisible: `lat_free`'s fallback chain (src/value.c:81-86, `if (g_heap && fluid_dealloc(...)) return; free(ptr)`) frees untracked blocks on any thread, and `lat_realloc_routed` falls through for growth — the probe verified both. If open: `lat_vec_push` the raw value, signal `cond_notempty` and all select waiters (identical wake code to `channel_send`, src/channel.c:55-64 — receivers cannot distinguish a moved value from a copied one), unlock. Disowning **before** the push is load-bearing (the receiver may recv-and-free immediately; a surviving registry entry is a double-free at sender teardown — ASan-validated ordering). Disowning **outside** the mutex (steps 4-6 precede the lock) means the O(heap) walk never stalls other channel users, and no un-disown path is needed because a disowned value restored to the binding remains fully functional.

Locking note: `channel_close` interleaving between disown and lock is handled by the closed-check-then-restore under the same mutex — the value cannot be enqueued into a closed channel, and cannot be lost.

Note on a pre-existing wart the tests must pin, not change: plain `channel_send`'s `false` return on closed channels is **ignored** by stackvm today (src/stackvm.c:2249 — silent success) while the tree-walker errors. Move-send errors-with-restore on all three backends; plain send keeps its current divergent behavior in v1 (harmonization is a filed ticket, §8).

### 3.2 Everything downstream is unchanged

- **Buffered channels:** moved values sit in `ch->buffer` plain-malloc-owned, on no thread's heap — like detached values today. Sender thread may exit with values buffered. `channel_release`'s drain (src/channel.c:21-36) frees leftovers on whichever thread drops the last ref, via the `lat_free` fallback chain.
- **recv and select:** zero changes. `channel_recv` (src/channel.c:69-96) memcpys out; the receiver's binding owns a malloc-backed value *in its original phase* (fluid arrives fluid and mutable — the headline; crystal arrives crystal). Select arm bindings (`env_define` eval.c:~10112; stackvm.c:~7883) take ownership normally. Receiver-side in-place mutation, growth, mixed-ownership containers, and cross-thread free are all probe-ASan-verified.
- **Receiver GC:** moved values are invisible to the receiver's registry-based sweep, reclaimed only by `value_free` — byte-for-byte the status quo for every channel payload today (detached values are equally unregistered). No re-registration in v1. Two invariants recorded in `include/memory.h`: (a) any future GC must preserve the `lat_free` registry-miss-falls-to-`free()` chain; (b) a moved-in value nested in receiver-heap structures is reclaimed only by teardown, never by sweep — long-lived receiver caches of moved values are leak-until-teardown, documented.
- **xfer_collect/value_free lockstep** (the permanent-maintenance hazard): `xfer_collect` lives in src/value.c adjacent to `value_free` with a tripwire comment on the ownership-site list; a **constructor-parity test** asserts, for every ValueType constructor, that `xfer_collect`'s set equals the instrumented registry delta during construction; and the serial `make asan` CI stage catches dynamic misses (a missed pointer = sender-teardown free while receiver holds = immediate ASan report in the cross-thread tests).

---

## 4. Use-after-move: complete read-site coverage (the "never silent" guarantee, made true)

The judges falsified the original coverage claim three ways: stackvm has fused opcodes that borrow slots without `OP_GET_LOCAL`; regvm has **no load chokepoint at all** (`EXPR_IDENT` compiles to an elided `ROP_MOVE` — src/regcompiler.c:585 `if (src != dst)` — and `ROP_ADDI` reads the variable's register in place, :639); and `value_is_truthy` has `default: return true` (src/value.c:1156-1172), so a leaked tombstone would silently steer an `if`. Resolution, per backend:

**Stack VM** — `type == VAL_MOVED` branch (one predictable compare) at every direct binding read, from the audited list of all 27 `frame->slots[` sites: `OP_GET_LOCAL`/`OP_GET_GLOBAL(_16)`/`OP_GET_UPVALUE`, the five fused borrows (src/stackvm.c:5046, 5548, 6019, 6114, 6438 — invoke-local ×2, set-index-local, index-local, get-field-local), `OP_INC_LOCAL`, `OP_APPEND_STR_LOCAL`, `OP_CHECK_TYPE`, and the binding-reading phase ops `OP_FREEZE_VAR`/`OP_SUBLIMATE_VAR`/thaw write-back/pressurize (the judge-flagged gap where `freeze(x)` would otherwise silently set a phase on a tombstone).

**Register VM** — locals are registers private to a frame; only same-function code reads them, and globals/upvalues *do* have load ops. So: checks at `ROP_GET_GLOBAL` and upvalue loads, plus **compiler-emitted checks for locals**: `regcompiler` tracks, per function, the set of local variables that are the target of any `move`/`is_moved` in that function, and emits a new cheap `ROP_CHECK_MOVED reg` before each read of those variables. Zero cost for every function (and every program) that never uses `move`; sound because register locals cannot escape the function. This replaces the original design's indefensible "regvm register flows fall to type-dispatch defaults" — which the judges proved meant silent truthiness, silent equality, and `"<moved: x>"` interpolating into strings.

**Tree-walker** — checks in `env_get` (src/env.c:101), `env_get_ref`/`env_get_ref_prehashed` (the `resolve_lvalue` path that bypasses `env_get` — judge-verified), and `resolve_lvalue` itself.

**Defense-in-depth, now genuinely unreachable:** with the above, a tombstone cannot flow into an expression, so the value.c terminal functions get *defensive* cases (truthy→false, eq→false, hash→0, repr→`"<moved: name>"`, `type_of`→`"moved"`) plus a debug-build assert — they exist for the debugger (`dap.c` gets an explicit display case so a DAP session showing a tombstoned slot renders `<moved: x>` instead of crashing) and for is_moved-style tooling, not as a safety net the language leans on. The json/yaml/toml serializers get defensive error cases.

**Tree-walker reentrancy rule** (judge hole: `garr.push(g())` where `g()` moves global `garr` while `resolve_lvalue` holds a pointer into the env entry): the in-place tombstone write keeps the entry's *address* stable (no map insertion → no rehash → no dangling borrow, see §5), but the *bits* change; therefore every eval.c mutation path that holds an env-entry pointer across user-code evaluation re-validates `entry->type` after argument evaluation and errors on `VAL_MOVED`. Stack VM is safe by instruction ordering (arguments are evaluated before the fused op borrows its slot). The adjacent stackvm edge — a global mutation whose write-back could overwrite a tombstone written mid-argument-evaluation ("lost move") — is pinned by a per-backend test as error-or-mutation-wins, documented as unspecified-but-memory-safe; it requires deliberately pathological code (`move` of the receiver from inside its own argument list).

**Error catalogue** (all deterministic, all catchable, post-catch state specified):

| Error | When | Binding after |
|---|---|---|
| `use of moved value 'x': sent on a channel at line N; assign a new value, or clone(x)/freeze(x) before sending to keep a copy` | any read of a tombstone | tombstone persists; revivable |
| same | double move | unchanged (still tombstone) |
| `cannot move 'x': it contains a Ref / closure / iterator` | classify, pre-commit | **untouched, fully usable** |
| `move requires a channel send; '<type>' is not a channel` | receiver check, pre-take | **untouched** |
| `cannot send on a closed channel` | post-disown, under mutex | **value restored; no tombstone** |
| `move is only valid as a channel send argument` / `move requires a variable` | compile (VMs) / eval (tree-walker) | n/a |
| `cannot move 'x' here: declare it inside the scope/select body, or move before the scope` | sub-chunk context | n/a |

The compile-vs-eval timing divergence for misplaced `move` (VMs reject before execution; tree-walker rejects when the line runs) is pinned by per-backend tests and documented — same class as existing divergences.

---

## 5. Per-backend binding surgery

- **Stack VM:** `OP_SEND_MOVE_VAR` + `OP_IS_MOVED_VAR`, appended past current numbering (`OP_RESET_EPHEMERAL`=98, `OP_HALT`=99). Take/restore/tombstone via the `OP_FREEZE_VAR` location-resolution logic with raw stores (never `stackvm_write_back` — §3.1). `stackvm_record_history` is called with the tombstone so `vm_record_history` variable tracking shows exactly when the binding was consumed. Invariant stated in code: no error may fire between take and push except the controlled restore paths, and the lifted value never sits on the VM value stack (so `stackvm_handle_error`'s free-less unwind — src/stackvm.c:154-178 — never strands it).
- **Register VM:** `ROP_SEND_MOVE_VAR` (register/upvalue/global targeting per `ROP_SUBLIMATE_VAR`), `ROP_IS_MOVED_VAR`, `ROP_CHECK_MOVED`. **Both** verifier passes extended (regvm.c:~7002 and ~7128), **and** `runtime_stubs.c`'s `regchunk_verify` stub kept consistent — commit `8610c31` exists because that stub drifted and broke the `clat-run` link; it is a named checklist item, not a hope.
- **Tree-walker:** new `env_take_in_place(env, name, LatValue *out)` — locates the entry **in its owning scope** (the `env_get_ref` walk), memcpys the value out, writes the tombstone into the *same entry*. Never `env_remove`+`env_define`: that pair lands the tombstone in the innermost scope (wrong scope → "undefined variable" instead of use-after-move, or shadow-resurrection) and the `lat_map_set` insertion can rehash the scope map, dangling outstanding `env_get_ref` borrows — both judge-verified bugs in the predecessor designs. In-place overwrite has neither failure mode. The send path special-cases `VAL_CHANNEL` receiver + `"send"` + `EXPR_MOVE` arg before generic argument evaluation (which would otherwise clone via `env_get`); restore-on-closed writes back through the same entry pointer.

---

## 6. Formats, bootstrap, thin runtime, WASM (the schedule-killing omissions, now budgeted)

Verified: both loaders use strict equality (`src/latc.c:585` for `.latc`, `:988` for `.rlatc`; `LATC_FORMAT`=`RLATC_FORMAT`=2 in include/latc.h:10-12) and `compiler/latc.lat:5299` hardcodes `write_u16_le(2)`. A naive bump bricks the self-hosted compiler's output, the checked-in `latc.latc`, and shipped `clat-run` artifacts.

**Policy: range-accepting loaders + conditional emission.**
- `LATC_FORMAT_MIN=2`, `LATC_FORMAT_MAX=3` (same for RLATC); both gates become range checks with the same error text for out-of-range.
- The C serializers emit version 2 unless the chunk graph contains a new opcode (`chunk_needs_v3` helper), version 3 otherwise. All move-free programs keep producing v2 files that old binaries still load.
- `compiler/latc.lat` is **untouched** (zero edits to the 5,344-line file): it keeps emitting v2, which the new loader accepts → **the bootstrap fixed point stays green by construction**, gated by an explicit CI test (latc.lat compiles itself; output recompiles input; diff clean).
- Self-hosted dialect: `latc.lat` does not know the `move` keyword, so a program using `move` fails **loudly** under the self-hosted compiler (unknown function/parse error) — never the silent copy-vs-move semantic fork the implicit design had. Documented; `latc.lat` move support is a filed follow-up.
- **clat-run thin runtime:** `src/transfer.c` added to `RUNTIME_SRCS` (Makefile:190-232), `runtime_stubs.c` audited for new symbols, the clat-run diff harness and the release matrix (Makefile:825-828, incl. Windows cross-build) re-validated. Two of the last five commits on main are clat-run link fixes; this gets its own stage-gate line.
- **WASM:** `transfer.c` and `channel_send_move` follow channel.c's `#ifndef __EMSCRIPTEN__` discipline (src/channel.c:46-65). Single-threaded moves are same-thread disown+push — harmless and correct. `make wasm` smoke test added to the gate.
- **ValueType/ABI:** `VAL_MOVED` appended last (enum currently ends `VAL_REF, VAL_ITERATOR` — include/value.h:30-32); extension ABI (`lattice_ext.h`) is additive; the verifier rejects `VAL_MOVED` in serialized constant pools, so old binaries can never load one.

---

## 7. Composition with the sibling efforts

**Crystal-by-reference (aliasing frozen values).** Binding-local tombstoning is exactly what composes: if crystals become atomically-refcounted aliases, `move(c)` transfers **one reference** bitwise — refcount unchanged, one binding tombstoned, every other alias keeps reading immutable data safely. Moving a crystal becomes an unconditional pointer handoff. The "aliases see moved-from invalidation" incompatibility from the earlier analysis dissolves because aliased crystals are never disowned, only handed off. **Contract imposed on that sibling:** (a) aliased crystal payloads carry a non-`REGION_NONE` region_id or a refcount header `xfer_collect` recognizes, so the collect walk classifies them as borrowed and never disowns/frees them — the hook exists since `xfer_collect` already skips non-`REGION_NONE`; (b) the refcount must be atomic (precedent: `channel_retain`'s `__atomic` ops). Mixed containers then work per-node: fluid spine disowned, crystal leaves ride bitwise, counted once.

**Crystallized layouts (contiguous frozen regions).** `transfer_classify` in src/transfer.c is the designed extension point. v1: region-backed subtrees → whole-value detach fallback (correct, slow, never wrong). When layouts land: a `region_transfer` branch performs arena-page handoff — sender's RegionManager unlinks the page set, the `LatValue` rides the same raw-push channel path, receiver adopts the pages (or they become orphan-malloc, freed by the same `lat_free` fallback chain). `freeze(m); ch.send(move(m))` then becomes an O(1) page transfer regardless of size. **Constraints imposed:** pages must be malloc-grained and self-contained (no interior pointers into other pages); region ids must be process-unique. **Constraint inherited:** because explicit move preserves phase, a moved layout arrives CRYSTAL with its read-optimized layout intact — no condensation machinery, and no phase-history bit is needed (that requirement existed only for sublimate-as-move's recv-side phase flip, which this design eliminated).

**End-state vocabulary:** freeze → crystal (immutable, copyable, eventually aliasable/contiguous); sublimate → gas (immutable, non-copyable); **move → the binding-level transfer that works on every phase**, fluids arriving fluid, crystals arriving as reference/region handoffs. The moved-from state is not a phase; the binding is simply empty until refilled.

---

## 8. Explicit descopes (each gets a JIRA ticket at Stage 4)

1. **Sublimated-send harmonization.** Today the three backends disagree on plain send (eval.c:13212 crystal-only with a fluid-scalar exemption; stackvm.c:2243 rejects only `VTAG_FLUID`, so sublimated/unphased deep-copy through; regvm.c:2516 crystal-or-unphased). The original design's "companion tightening" (reject sublimated plain-sends) is **dropped from v1** — it was a hidden breaking change requiring three-way harmonization. v1 changes plain-send behavior on zero backends; `tests/test_stdlib.c:2662` stays green untouched.
2. **Unphased ref/iterator/closure leakage through plain send** (pre-existing unsoundness via `value_detach` sharing). Filed as a bug; move's hard-error rule does not extend to plain send in v1.
3. **Hash-indexed FluidHeap registry** to kill the O(heap) disown floor (~0.13 ms at 64k entries). Pure perf, no semantic change.
4. **Region page handoff** — lands with crystallized layouts into the `transfer_classify` seam.
5. **Crystal refcount handoff** — lands with crystal-by-reference.
6. **`compiler/latc.lat` move support** and general move-expression positions (`let y = move(x)` as cheap rename).
7. **Moves of enclosing locals inside scope/select bodies** (requires synchronized sub-chunk export semantics).
8. **Plain-send closed-channel silent success on stackvm** (src/stackvm.c:2249 ignores the return) — pre-existing; pinned by test, harmonization ticketed.
9. **Reaction on move** — none in v1; adding one later is a flagged behavior change.

---

## 9. Staged implementation plan (tests first per stage; three-backend gate `make test` / `make test-tree-walk` / `make test-regvm` plus **serial** `make asan` — `-j` races the asan clean step — at every stage)

**Stage 0 — Format/version groundwork. User-visible: nothing. Bootstrap proof.**
*Tests first:* loader accepts v2 and v3 `.latc`/`.rlatc`, rejects 1 and 4; round-trip serialize/load at both versions; **bootstrap fixed-point test** (clat compiles `compiler/latc.lat`; latc.lat compiles a corpus + itself; outputs load and run; self-compilation diff stable); clat-run loads both versions.
*Files:* include/latc.h (`*_FORMAT_MIN/MAX`), src/latc.c (two range gates, conditional-version serializers + `chunk_needs_v3`), Makefile (CI hooks). `compiler/latc.lat`: **zero edits.**
*~2 days.*

**Stage 1 — Transfer core, no language surface. User-visible: nothing.**
*Tests first:* promote the probe's 7 tests from the worktree spike into `tests/test_transfer.c` proper (cross-thread move of 100-entry map with registry accounting; interned-borrow; refusal kinds; benchmarks); add the constructor-parity test (per-ValueType: collect set == instrumented registry delta); add closed-channel-restore and mixed-ownership-teardown C tests; serial ASan.
*Files:* new src/transfer.c + include/transfer.h (`xfer_collect` co-located/cross-referenced with `value_free`, `transfer_classify`, `fluid_disown_set`, `channel_send_move` with the closed-check-restore contract), src/memory.c (list-head access + live-count/byte decrement), Makefile (`TEST_SRCS`, **`RUNTIME_SRCS`**), runtime_stubs.c audit, `#ifndef __EMSCRIPTEN__` discipline.
*~3 days.*

**Stage 2 — `VAL_MOVED` + complete read-path checks. User-visible: nothing (tombstones cannot yet be constructed from Lattice).**
*Tests first:* C-level tombstone tests across every terminal (free/clone/repr/eq/hash/truthy/gc/type_of/dap-display/serializer-reject); read-site checks raise at each of the audited sites; perf benchmark harness recording baseline.
*Files:* include/value.h (append `VAL_MOVED`, `as.moved`), src/value.c (cases + debug asserts), src/gc.c, src/dap.c, src/json.c/yaml_ops.c/toml_ops.c/type_ops.c (defensive cases); src/stackvm.c (checks at the 27-site audit list incl. fused borrows and `OP_FREEZE_VAR`/`OP_SUBLIMATE_VAR`/thaw/pressurize); src/regvm.c (`ROP_CHECK_MOVED` handler + global/upvalue load checks); src/env.c + src/eval.c (`env_get`, `env_get_ref*`, `resolve_lvalue`, re-validation rule in mutation paths).
*Gate:* benchmark suite <1% regression on all three backends, or the check strategy is revisited before Stage 3.
*~4-5 days.*

**Stage 3 — The language surface. User-visible: `ch.send(move(x))` and `is_moved(x)` on all three backends.**
*Tests first (the matrix):* {local, upvalue-open, upvalue-closed, global} × {zero-copy kinds, channel payload, scalar, ephemeral-fallback, region-fallback, ref/closure/iterator error} × {unbuffered, buffered, recv-in-select-arm, closed-channel-restore, non-channel receiver}; double-move; revive-by-assignment; send-in-loop; `x += 1` after move errors; freeze/thaw/sublimate on tombstone error; scope/select body restriction errors (and body-local moves succeed); try/catch semantics incl. post-catch state; `is_moved`; REPL provenance + tombstoned-global persistence across entries; per-backend pins for the upvalue divergence, the compile-vs-eval misplaced-move timing, and the pathological move-during-enclosing-mutation case; buffered-channel teardown with unconsumed moves; receiver-mutation under ASan; bootstrap fixed point re-run; `move`-using source under latc.lat fails loudly.
*Files:* src/lexer.c (TOK_MOVE, TOK_IS_MOVED beside the keyword table), src/parser.c (~:1456, beside TOK_SUBLIMATE; ident-only operand), include/ast.h (EXPR_MOVE/EXPR_IS_MOVED reusing `freeze_expr`); src/stackcompiler.c (`OP_SEND_MOVE_VAR`/`OP_IS_MOVED_VAR` emission, position + sub-chunk-context rules); src/stackvm.c (fused handler per §3.1, history hook); src/regcompiler.c (ROP twins + per-function moved-locals analysis emitting `ROP_CHECK_MOVED`), src/regvm.c (handlers, **both** verifier passes), runtime_stubs.c; src/env.c (`env_take_in_place`), src/eval.c (send special-case, scope-depth restriction, position errors); src/chunk.c verifier; `chunk_needs_v3` wiring goes live.
*Gate:* full three-backend suite + serial ASan + clat-run diff harness + `make wasm` smoke + Windows cross-build + bootstrap fixed point.
*~7-9 days.*

**Stage 4 — Tooling, docs, release. User-visible: diagnostics polish.**
`--warn-move-copy` (fires on the ephemeral/region fallback), REPL message variants, docs (phase-matrix table, recipes incl. `let v = arr.pop(); ch.send(move(v))`, perf model, the upvalue-divergence example), `generate_docs` annotations, `LATTICE_VERSION` bump, release notes (one intentional surface addition, zero behavior changes to existing programs), file the §8 tickets, close with the project's three-backend test rule.
*~2 days.*

**Total: ~18-21 dev days (~4 working weeks).** This is deliberately ~2× the original design's 7-10 days, per the schedule-focused verdict: the delta is exactly the formerly omitted deliverables — Stage 0 (formats/bootstrap), clat-run/WASM/Windows in the Stage 1/3 gates, the 27-site stackvm audit + regvm `ROP_CHECK_MOVED` codegen, the dual-verifier/stub consistency work, and the divergence-pinning test matrix. The only probe-de-risked line is Stage 1; everything else is priced as new work.

---

## 10. Judge-hole disposition index

Every hole from the six verdicts, by disposition: **(R)** resolved in this design, **(D)** explicitly descoped with ticket, **(P)** pinned-by-test documented divergence.

Fallback-detach unsound for ref/closure/iterator → **R** hard error, collect-then-commit (§1, §3.1-3). `.send(move(x))` dispatch hijack / non-channel receiver / PIC interaction → **R** channel-check-before-take in one fused opcode; PIC untouched (§3.1-1). `stackvm_write_back` double-free + tombstone/push ordering incoherence → **R** take-and-blank raw stores; restore-on-closed; same-thread binding visibility makes post-take ordering race-free (§3.1-4/7, §5). Closed-channel value loss → **R** restore, never lose (§3.1-7). regvm no-chokepoint / silent truthy/eq/repr → **R** compiler-emitted `ROP_CHECK_MOVED` + global/upvalue checks; terminal defaults made defensive-only (§4). Fused slot-borrow opcodes bypass GET → **R** 27-site audit list (§4). freeze/sublimate/pressurize read tombstones silently → **R** checked (§4). env_remove wrong-scope tombstone + rehash dangling borrows → **R** `env_take_in_place` (§5). eval reentrancy (`resolve_lvalue` across arg eval) → **R** re-validation rule + test (§4). scope/select sub-chunk clone-vs-real divergence → **R** body-local-only restriction (§1-3). Upvalue VM-vs-tree-walker divergence → **P** (§1-4). Misplaced-move compile-vs-eval timing → **P**. version gates / latc.lat hardcoded 2 / bootstrap → **R** range loaders + conditional emission, latc.lat untouched (§6). `.rlatc` second format + dual verifiers + runtime_stubs drift → **R** named work items (§5, §6). clat-run / WASM / Windows omissions → **R** staged gates (§6, §9). Catchability + post-catch state → **R** specified table (§4). Introspection/debugger/REPL line numbers → **R** `is_moved`, dap case, REPL provenance variant (§2). Self-send channel cycle leak → **P** documented known limitation (§1). Per-load perf tax unmeasured → **R** Stage 2 benchmark gate (§9). O(heap) disown floor → **D** ticket 3 (§8). xfer_collect/value_free lockstep → **R** co-location + constructor-parity test + ASan gate (§3.2). FluidHeap accounting drift → **R** decrement in `fluid_disown_set` (§3.1-6). Sublimated-send tightening as hidden break → **D** ticket 1 (§8). `test_channel_crystal_only_send` breakage → **R** plain send untouched; test stays green (§8). VAL_MOVED-in-containers / serialization → **R** by the binding-only invariant, now upheld by complete read coverage + verifier constant-pool rejection (§2, §6). Moved-values-invisible-to-receiver-GC → **P** status-quo-equivalent, invariant recorded (§3.2). Ephemeral-subtree interior-pointer mechanics → **R** whole-value detach fallback, no partial rewrite (§1 matrix). UNPHASED matrix row → **R** move is phase-agnostic; plain send unchanged (§1, §8). Effort underestimate → **R** re-priced at 18-21 days with the omitted deliverables as named stages (§9).