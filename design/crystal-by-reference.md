All load-bearing claims verified. Writing the final design document now.

---

# Crystal-by-Reference Sharing in Lattice — Final Design (v1)

**Status:** Approved architecture, synthesized from three candidate designs ("refcount-cow", "region-ownership", "minimal-borrow") and six adversarial reviews. Every soundness hole raised by the judges is resolved or explicitly scoped out below.

---

## Status (2026-06-11) — Stages 0–6 complete

All six stages are implemented and green across all three backends (suite 1987 ×3, ASan, TSAN, differential force-copy oracle, Windows/wasm/clat-run platform gates).

**Commits per stage:**

| Stage | Commits | Tickets |
|---|---|---|
| 0 — Phase-guard hardening | `7342137`, `a491d97`, `de96339`; channel-send reconciliation `d95efce`, `5219777`, `6406919`, `4d80ec4` | LAT-441, LAT-442, LAT-443 |
| 1 — Closure `upvalue_count` migration | `6e81f18` | LAT-446 |
| 2 — Refcounted regions + tree-walker | `4900d0f` (Round A, dormant infra), `6d50d03` (Round B, sharing live) | LAT-449 |
| 3 — Stack VM | `65bbdf7` | LAT-452 |
| 4 — Register VM | `cf77625` | LAT-456 |
| 5 — Concurrency hardening | `afcf0fd` | LAT-457, LAT-448, LAT-450 |
| 6 — Perf validation + docs | benchmark suite (`make bench-cbr`, `scripts/bench_cbr.py`, `benchmarks/cbr_*.lat`), results in `benchmarks/RESULTS.md`, README/doc updates | LAT-459 |

**Measured results (Stage 6, M3 Max, commit `afcf0fd`; full tables in `benchmarks/RESULTS.md`, regenerate with `make bench-cbr`):** aliasing a frozen 10k array is ≥1252x faster on the stack VM and ≥1256x on the regvm (shared cells at clock resolution — lower bounds), 6.2x on the tree-walker; arg-pass of a frozen map 5.9x/4.8x/11.4x (stack/tree/regvm); channel streaming of frozen payloads 9.2–14.5x; spawn over a frozen 10k dataset 138x on the tree-walker (the VMs were already cheap: 2–3x); plain reads are sharing-neutral on the VMs. The cost case: the fix-freeze loop (1000 clone+freeze+read iterations over a fresh 10k array) runs 270 ms on the stack VM (`benchmarks/RESULTS.md`) vs 146 ms under `--no-regions` (measured separately — the FORCE_COPY oracle still materializes, so it cannot serve as the tag-flip baseline) — ≈1.9x for the whole iteration, with the freeze-op-only ratio somewhat higher since the per-iteration clone cost is common to both; one-time, and within the `make bench-freeze-gate` budget (190 ms baseline x3.0). The threshold sweep is consistent with `value_worth_regionizing`: below the 32-byte string cutoff both modes execute identical legacy code (the sweep cannot distinguish them there — sub-cutoff deltas in RESULTS.md are harness noise); sharing wins appear from 32 bytes up and grow with alias count; arrays win at every size (even 1-element arrays at 64 aliases: 32 ms vs 51 ms), supporting the no-cutoff always-regionize container policy. A `--variant` rebuild sweep of `REGION_SHARE_MIN_STR_LEN` (supported by `scripts/bench_cbr.py`) is the rigorous cutoff validation and has not been run for the checked-in RESULTS.md.

**Deviations from this design:**

1. **Default-on, not flag-first.** §4 Stage 6 planned shipping behind `LATTICE_SHARE_CRYSTALS=1` for one release before flipping the default. Instead, sharing went default-on as each backend landed (Stages 2–4), with `--no-regions` as the wholesale opt-out and the force-copy oracle as the permanent safety net. The formal rollout decision (LAT-459, 2026-06-12): default-on confirmed, plus `LATTICE_SHARE_CRYSTALS=0` as a runtime env kill switch gating `value_freeze_to_region` directly — one chokepoint covering all backends, `clat-run`, and embedded hosts.
2. **Oracle spelling.** The differential oracle env var is `LATTICE_FORCE_COPY=1` (disables the borrow fast paths; semantics must be bit-identical), not `LATTICE_SHARE_CRYSTALS`. It is exercised in CI via `make test-force-copy`.
3. **Freeze-except representation.** H3 stated freeze-except parents "stay FLUID". The implementation kept the crystal-parent model: a freeze-except parent is `VTAG_CRYSTAL` with the exempted entries recorded fluid in `key_phases`/`field_phases`. Such parents are always *legacy* crystals — an exempt (writable) entry implies the container never regionizes, and `region_tag_recursive` strips `key_phases` at materialization, so the predicate's invariant holds (asserted in `phase_check_index_write`).
4. **Scalars excluded from regionization.** §2.8 item 2's mitigation is codified as `value_worth_regionizing` (src/value.c): scalars/ranges never regionize, and strings under `REGION_SHARE_MIN_STR_LEN` (32 bytes) stay legacy — copying them is cheaper than refcount traffic. Validated by the Stage 6 threshold sweep.

**Open follow-ups:**

- LAT-453 — tree-walker `--gc-stress` crashes on freeze-except/anneal/grow patterns (pre-existing)
- LAT-454 — `OP_APPEND_STR_LOCAL` lacks crystal phase check (`+=` on frozen string locals)
- LAT-455 — `--stats` is a no-op on the stack VM backend
- LAT-458 — verify Ref-proxy mutators unshare borrowed inner values (stackvm + tree-walker)
- LAT-460 — cross-thread Ref mutation: free-direction double-free (tree-walker) + design decision
- LAT-459 — rollout decision (user call; documented in the ticket)

---

## 0. Verification of load-bearing claims (performed against the working tree)

Before synthesis, I re-verified the judges' most consequential findings against `/Users/alexjokela/projects/lattice`:

| Claim | Verified | Evidence |
|---|---|---|
| Region sentinels are `(size_t)-1..-4` | YES | `include/value.h:47-50` |
| `value_free` is a memset no-op for `region_id != (size_t)-1` | YES | `src/value.c:1062-1066` ("arena owns everything") |
| `value_thaw` already deep-clones, then flips phase on the clone only | YES | `src/value.c:576-589` (VAL_REF special case included) |
| `set_phase_recursive` never updates map `key_phases` on freeze (stale-hole bug) | YES | `src/value.c:526-567` — struct `field_phases` updated, map `key_phases` untouched |
| `set_phase_recursive` recurses through `VAL_REF` into shared `LatRef` inner | YES | `src/value.c:564-566` |
| `set_region_id_recursive` has **no `VAL_TUPLE` case** and unconditionally writes `region_id` on closures + walks `captured_env` as `Env*` | YES | `src/eval.c:447-485` |
| **`freeze(a); a[0]=99` silently mutates the frozen array in the stack VM AND tree-walker** (only regvm rejects) | YES — **ran it** | `/tmp/mut_test.lat` → `[99, 2, 3]`, exit 0 on both |
| `freeze(m); m["a"]=99` silently mutates a frozen map in the stack VM | YES — ran it | `{"a": 99}`, exit 0 |
| **Frozen `Buffer.write_u8` silently mutates in stack VM AND regvm** (no phase checks anywhere in buffer natives) | YES — ran it | prints `255`, exit 0 on both; `grep` confirms **zero** `VTAG_CRYSTAL` checks in `src/builtin_methods.c` (which has 54 `value_deep_clone` sites) |
| Compiled closures overload `region_id` as upvalue count | YES | `src/stackvm.c:4101, 4153, 4189, 4883, 4935, 5103, 5162, 5398, 5445, 5601`; `src/latc.c:720, 894` (serializer AND loader) |
| `OP_GET_FIELD` steal pattern writes `VAL_NIL` into the container | YES | `src/stackvm.c:4704-4708` |
| `OP_MARK_FLUID` flips phase in place on the peeked value | YES | `src/stackvm.c:6861-6864` |
| `OP_CLONE` = `value_deep_clone` preserving phase | YES | `src/stackvm.c:6850-6856` |
| `channel_send` detaches via `value_detach` for thread independence | YES | `src/channel.c:38-44` |
| `CrystalRegion` has no refcount today; `freeze_to_region` is tree-walker-only and gated by `no_regions` | YES | `include/memory.h:98-103`, `src/eval.c:501-516` |
| regvm has `ROP_THAW_FIELD` and permits crystal-map mutation through `key_phases` holes | YES | `src/regvm.c:2862, 3602-3611, 5954-5962, 6630` |

The judges were right about the most damning point: **the design premise "phase checks already prevent crystal mutation" is empirically false today** in two of three backends. That dictates the staging below: guard hardening is a *prerequisite release*, not an audit item.

---

## 1. Foundation choice

**Base: Design 2 (sealed-region / region-ownership)** — process-global, sealed-immutable `CrystalRegion`s with atomic refcounts and pointer-valued region ids. It has the right lifetime story (deterministic, cross-thread-correct refcounting; no census, which Design 3's judges demonstrated has a concrete UAF under reentrant execution — `OP_DEFER_RUN` + builtin callbacks executing `OP_RESET_EPHEMERAL` while borrowed handles sit in unrooted C locals).

**Grafted from Design 1 (refcount-cow):**
- **Self-contained regions** — freeze force-copies nested crystals into the new region instead of recording cross-region dep edges. No region ever points at another region. Refcounting is trivially cycle-free; no dep-edge retain lists; pinning is per-region only. (Design 2's dep-edge DAG is deferred indefinitely — it was a perf nicety with a correctness tax.)
- **Unconditional copy-on-thaw with an explicit force-copy clone variant** (not a TLS flag — see §3.4).
- Channel send = retain + enqueue handle; spawn = retain frozen bindings, copy fluid ones.

**Grafted from Design 3 (minimal-borrow):**
- **The safe-fallback property**: values that cannot be soundly regionized keep today's tag-flip "legacy crystal" semantics (`region_id = REGION_NONE`, clone-on-copy everywhere). The sharing predicate simply never matches them. This is the single move that retires the worst judge holes (closures, refs, iterators, channels, sublimated members) *by construction* instead of by audit.
- The debug `mprotect(PROT_READ)` tripwire on sealed region pages, and the differential force-copy oracle build mode.

**Rejected:** Design 3's per-thread census lifetime (reentrancy UAF, regvm has no root discipline); Design 1's numeric-range `IS_REGION_PTR` predicate (unsound on wasm32); Design 2's dep edges (v1).

---

## 2. Core mechanism

### 2.1 Two kinds of crystal

After this feature, the system permanently contains two crystal classes (one judge called this out as inevitable; we make it explicit and documented):

1. **Shared crystal** — phase `VTAG_CRYSTAL`, `region_id` = tagged `CrystalRegion*`. Backing store lives entirely inside one sealed, refcounted, process-global region. Aliased by O(1) bitwise handle copy + retain. Contains **only pure data**: INT, FLOAT, BOOL, UNIT, NIL, RANGE, STR, ARRAY, STRUCT, MAP, SET, TUPLE, ENUM, BUFFER. Interned strings inside regions stay `REGION_INTERNED` (existing precedent, `src/value.c:312-317`).
2. **Legacy crystal** — phase `VTAG_CRYSTAL`, `region_id == REGION_NONE`. Exactly today's tag-flipped value. Full pass-by-value clone semantics everywhere. Produced when the value is unshareable (see 2.3) or in `--no-regions` mode.

Fluid/unphased values are untouched: copy semantics bit-for-bit identical to today.

### 2.2 Region identity: low-bit-tagged pointer

```c
/* include/value.h */
#define REGION_IS_SHARED_ID(rid)  (((rid) & 1u) == 1u && (rid) != REGION_NONE && (rid) != REGION_INTERNED)
#define REGION_PTR(rid)           ((CrystalRegion *)((rid) & ~(size_t)1))
#define REGION_TAG(ptr)           (((size_t)(ptr)) | 1u)
```

- `CrystalRegion*` is malloc'd, ≥8-byte aligned → low bit always 0 → `ptr|1` is unambiguous.
- Sentinels: `-1` and `-3` are odd, excluded by name; `-2`/`-4` are even, excluded by the bit test. A `memset`-zeroed handle (`region_id == 0`) has low bit 0 → never classified shared → **double-free of the same handle stays forgiving**, exactly as today.
- Works on wasm32 (alignment, not address range) and Windows. Resolves the pointer-range fragility hole from Designs 1 & 2.
- **Prerequisite:** compiled closures must stop overloading `region_id` as upvalue count (Stage 1) — after migration closures carry `REGION_NONE` and a small odd upvalue count can never be misread.
- The tree-walker's numeric per-thread region ids are **retired entirely**; after Stage 2 the only `region_id` values in the system are the four sentinels and tagged pointers. (Numeric ids are always even? No — they were counters. They're gone, so it doesn't matter.)

`CrystalRegion` gains `_Atomic size_t rc;`. `RegionManager` shrinks to a mutex-protected global registry used only for stats, leak diagnostics, and process-exit teardown; **retain/release never touch it**. Retain = `fetch_add(relaxed)`; release = `fetch_sub(acq_rel)` + acquire fence + O(1) page free at zero — matching the channel refcount precedent (`src/channel.c:19-22`). O(1) page free is *sound* because regions contain only pure data (2.3): no nested channels, refs, iterators, or envs to finalize. This resolves the "O(1) teardown leaks embedded resources" hole from all three reviews of Design 2.

### 2.3 Shareability classification at freeze (the scope-down that kills the worst holes)

`value_freeze_to_region()` (new, in `src/value.c`, shared by all backends) begins with a cheap recursive pre-scan `value_is_shareable(v)`:

**Unshareable if the value transitively contains:** `VAL_CLOSURE` (either flavor), `VAL_REF`, `VAL_ITERATOR`, `VAL_CHANNEL`, any `VTAG_SUBLIMATED` member, or (defensively) anything already `REGION_EPHEMERAL`-backed that can't be copied out (it can — ephemeral data force-copies into the region; this clause is about correctness of the scan, not a rejection).

- **Unshareable → fall back to today's `value_freeze()` tag flip.** Behavior identical to current releases. No region, no sharing, no new hazards.
- **Shareable → materialize:** create region, arena-clone with **force-copy** (nested shared crystals are copied *into* the new region — self-contained invariant; nested legacy crystals likewise), then run a *rewritten* `set_region_id_recursive` that (a) **adds the missing `VAL_TUPLE` case**, (b) handles map/set entries, enum payloads, struct fields, buffers, strings, (c) **normalizes phase metadata**: clears stale `key_phases`/`field_phases` (everything in a region is uniformly crystal — also fixes the verified `freeze-except → refreeze` stale-key_phases bug as part of Stage 0/2), and (d) never encounters closures/refs (excluded by the scan), eliminating the `ObjUpvalue**` type-confusion crash and the upvalue-count clobber.

What this scope-down costs, stated honestly: **structs with method closures — the idiomatic Lattice OOP pattern — do not get O(1) sharing in v1.** They keep exactly today's semantics and cost. Frozen config maps, datasets, arrays, buffers, strings, enums — the channel/spawn payloads the feature targets — do. Closure sharing is a designed follow-up (it needs the closure-struct refactor plus an upvalue immutability story; see §6).

Freeze of an already-shared crystal is idempotent: retain + return the same handle, O(1).

### 2.4 Where aliasing happens: the clone primitives, with an explicit force-copy parameter

The borrow fast path goes at the top of all three clone primitives — **including `value_deep_clone`** (this is where I side with Design 1 over Design 3, because it's what makes spawn and the tree-walker win automatically):

```c
/* top of value_deep_clone, value_clone_fast (stackvm.c:239), rvm_clone (regvm.c:387) */
if (v->phase == VTAG_CRYSTAL && REGION_IS_SHARED_ID(v->region_id)) {
    crystal_region_retain(REGION_PTR(v->region_id));
    return *v;   /* bitwise handle copy */
}
```

`rvm_clone_or_borrow` (`regvm.c:381`) inherits it via `rvm_clone`. Because every read path in all three backends funnels through these (verified by the copy-site maps: `env_get` at `env.c:105/113`, `OP_GET_LOCAL/GLOBAL`, `OP_INDEX*`, `OP_ITER_NEXT`, call-arg binding, returns, `stackvm_export_locals_to_env:829`, `ROP_GETFIELD/GETGLOBAL/CALL`, spawn `env_clone`), aliasing lands at ~300 sites with three edits. A nested shared crystal inside a fluid container is retained when the fluid parent is cloned (the recursion hits the fast path) — Design 3's regvm "nested sharing doesn't materialize" gap is auto-resolved.

The Design-3 judges' correct objection — *thaw/detach/freeze must never receive an alias* — is resolved **without a TLS flag** (the `g_copy_out` reentrancy hazard one judge flagged): the clone walker becomes `value_clone_impl(const LatValue *v, bool allow_share)`, with:

```c
LatValue value_deep_clone(const LatValue *v)  { return value_clone_impl(v, true);  }
LatValue value_copy_out(const LatValue *v)    { return value_clone_impl(v, false); } /* recursive force-copy */
```

`allow_share` is threaded through the recursion as a parameter — reentrant evaluation (reactions, cascades firing user code mid-thaw) cannot observe or corrupt it. Force-copy callers, exhaustively:

1. **`value_thaw`** — `value_copy_out` + `set_phase_recursive(FLUID)` on the private copy + release the consumed handle. Region memory (including phase tags inside arena pages) is **never written**. All other aliases, any thread, observe nothing. Unconditional — no `rc==1` in-place optimization, ever, for Design 1's correct reason: arena-interleaved buffers can't be realloc'd, so a thawed-in-place array could never grow.
2. **Freeze materialization** (arena-active clone) — self-contained regions.
3. **`OP_CLONE` / `clone()`** — switched from `value_deep_clone` to `value_copy_out`. Decision: `clone()` keeps its meaning of *guaranteed physically independent copy*, and becomes the documented escape hatch for the whole-region-pinning problem ("extracted one element of a huge frozen dataset? `clone()` it to release the region").
4. **Extension/FFI boundary (`src/ext.c`)** — args marshalled to extensions are copied out; previously-compiled extensions never see region handles. Resolves the extension-ABI hole with a small perf tax instead of an ABI break.
5. **`value_detach`** — for *legacy* crystals and unphased values, unchanged (it must copy). For shared crystals, detach = retain + bitwise copy: region pages are plain global malloc, never tracked by any thread's `FluidHeap`, so they already survive sender-thread teardown — the entire reason detach exists doesn't apply.

`value_free` (`src/value.c:1062`): the no-op branch splits — `REGION_IS_SHARED_ID(rid)` → `crystal_region_release` + memset; other non-`REGION_NONE` sentinels → memset as today. **The release branch keys on the region_id tag alone, NOT on phase** — so a handle whose top-level tag was flipped (sublimate, mark-fluid before its guard fires) still releases correctly and can't leak. The *borrow* branch keys on tag **and** `phase == VTAG_CRYSTAL` — a phase-anomalous handle copies (safe) rather than aliases.

### 2.5 In-place writers: the unshare rule

`value_unshare(LatValue *v)`: if shared → `*v = value_copy_out(v)` + release original. Required before any in-place write to a possibly-shared handle. The complete audited list (every judge-found site included):

| Site | Treatment |
|---|---|
| `OP_MARK_FLUID` (`stackvm.c:6861`) | unshare first |
| `OP_SUBLIMATE` / `OP_SUBLIMATE_VAR` (`stackvm.c:~7171`) | unshare first (sublimated = non-copyable; can't be a view of shared memory) |
| `borrow()`/`crystallize()` phase restores (`eval.c:9140-9214`) | route through thaw/freeze primitives (copy-out / re-materialize); audit each `.phase =` write |
| Re-freeze of a shared crystal (`OP_FREEZE`, cascades) | short-circuit: retain + return; `set_phase_recursive` gains a debug assert it is never called on a shared handle |
| `rt_freeze_cascade` mirror/inverse bonds (`runtime.c:111-151`) | mirror: if target shareable → `value_freeze_to_region`; if target already shared → no-op retain; inverse: routes through `value_thaw` (already copies). **runtime.c is in scope** (it was missing from Design 2's plan — judge hole) |
| Steal patterns: `OP_GET_FIELD` struct/map/tuple/enum (`stackvm.c:4704-4760`), `OP_INVOKE`/`OP_INVOKE_LOCAL` self extraction | guard: if container shared → push bitwise copy of element + **retain** (element is in the same region by the self-contained invariant — now true for tuples too) + `value_free(container)` (= release); skip the NIL write |
| `OP_FREEZE_FIELD` / `OP_FREEZE_EXCEPT` / `ROP_FREEZE_FIELD` / tree-walk equivalents (`stackvm.c:7217-7396`, `regvm.c:6606-6745`, `eval.c:8637-8714`) | add the **crystal-parent guard the VMs are missing** (eval.c has it; VMs don't — verified judge hole): on an already-crystal parent → error ("already frozen"), matching eval.c. On a fluid parent: unchanged — and a fluid parent is never shared, so no arena writes. A shared crystal field stored *inside* a fluid parent is replaced by slot assignment (release old handle), never written through |
| Partial thaw: `thaw(s.field)`, `ROP_THAW_FIELD` (`regvm.c:6641`) | **copy-on-partial-thaw**: if parent is shared → `value_unshare(parent)` (whole parent copies out of the region), then apply field-phase mutation to the private copy, rebind. If parent is legacy crystal → today's behavior unchanged |
| `OP_ADD_LOCAL` string-append fast path (`stackvm.c:~8180`) and regvm in-place concat (`regvm.c:~3014`) — judges found these do `region_id = REGION_NONE` surgery | both are guarded on the value being mutable/REGION-owned; add explicit `REGION_IS_SHARED_ID` branch → unshare (these only fire on fluid strings in practice, but the guard is one line) |
| Mutating builtin/native methods (`builtin_methods.c` buffer write family, sort/fill/etc.) | **Stage 0 closes these as phase errors** (see §4 Stage 0). After Stage 0, every mutating method rejects crystals; shared values are always crystal; therefore no native can write through a shared handle. The dispatchers additionally gain a debug assert: no mutation path reachable with `REGION_IS_SHARED_ID` |

Backstop for everything missed: **debug builds `mprotect(PROT_READ)` region pages after seal** (regions are page-based arenas — cheap). Any stray write segfaults loudly in CI instead of corrupting silently. (`VirtualProtect` on Windows; on wasm the tripwire is unavailable — wasm runs the same test suite natively first.)

### 2.6 Lifetime, GC, and the leak question

- Per-thread fluid mark/sweep untouched. `gc_mark_value`'s early return for `region_id != REGION_NONE` (`src/gc.c:127`, `eval.c:225-236`) stays — GC never traverses or frees region memory; no cross-thread mark races.
- Tree-walker `gc_cycle` **stops freeing regions** via `region_collect`; refcount is the sole reclaimer (it is the only mechanism that can see cross-thread aliases).
- **The GC-sweep leak hole (all three designs' judges):** `fluid_sweep` frees raw blocks without running destructors, so a swept fluid container holding a shared-crystal handle never releases its rc. Resolution, three layers: (1) the VMs (default backends) use eager `value_free` discipline — sweep-reclaim of live handles is already a bug there; (2) **the tree-walker's reachability pass is retained as a debug-mode leak detector**: it computes the reachable region set exactly as today, and any region with `rc > 0` not in the set is reported (never freed) — converting today's silent mop-up into a loud diagnostic; (3) a debug global region counter + exit report. Accepted residual: a leaked fluid cycle pins its regions in release builds — documented, same class as today's leaked-cycle behavior, now with better diagnostics.
- Tests asserting region stats (`tests/test_memory.c:151-264, 377-425` use `region_collect` directly; `tests/test_eval.c:875/896/1117` assert `gc_swept_regions`/`region_live_count`) **will be updated**, not preserved — `region_collect` survives as the debug detector so the C unit tests are adapted to it plus new rc-lifecycle tests. The "suite passes unmodified" claim from all three candidates is officially walked back: Stage 0 also changes behavior of programs exploiting the missing guards (that's the point).

### 2.7 Concurrency

- **Channels:** send of a shared crystal = retain + enqueue the 64-byte handle under the existing channel mutex (`channel.c:46` — the mutex acquire/release is the happens-before edge giving the receiver visibility of region contents *and* of the retain). Legacy crystals and unphased values keep the `value_detach` deep-copy path — this also resolves the "UNPHASED values are sendable" guard subtlety (`stackvm.c:2243` rejects only `VTAG_FLUID`): classification at the send site is by `REGION_IS_SHARED_ID`, never by phase alone. recv transfers the count; channel teardown releases buffered values; last release may run on the receiver thread (freeing global malloc pages cross-thread is fine).
- **Spawn:** `create_child_evaluator` / `stackvm_clone_for_thread` / `regvm_clone_for_thread` keep their structure; the env clone now retains shared crystals and deep-clones only fluid bindings — spawn cost becomes proportional to mutable state. `env_detach_values` (`env.c:61-72`) skips shared handles. LAT-430 teardown ordering untouched. Isolation semantics unchanged: shared data is immutable, fluid data is copied; no mutation propagates between threads, by construction.
- **Atomics:** C11/`__atomic` builtins (already used by channels, already building under mingw); wasm is single-threaded so they compile to plain ops. The retain-before-visibility rule ("a retain must happen-before the handle becomes visible to another thread; channel mutex and `pthread_create` provide it") is documented in `memory.h`. The ext boundary copies out (2.4 #4), closing the "natives smuggling handles cross-thread" path.
- **No non-atomic foreign refcounts ever become cross-thread-reachable** — LatRef/Env/iterator are excluded from regions (2.3), so their plain `++` refcounts stay thread-confined exactly as today. No atomicization of `ref_retain`/`env_retain` needed in v1.

### 2.8 What is observably different (honest list)

1. Programs exploiting the missing guards (`freeze(a); a[0]=99`, frozen-map assign, frozen-buffer writes) **become runtime errors after Stage 0** — in all three backends, uniformly. This is a bug fix shipped ahead of the feature.
2. VM `freeze()` of a shareable container: O(n) tag walk → O(n) region copy (one-time; buys O(1) forever after). Every `fix x = <container>` pays it (`OP_FREEZE` is emitted for fix bindings). Mitigation: scalars/short strings skip regionization (stay legacy — copying them is cheaper than refcount traffic); benchmark gate in Stage 6; freeze-heavy-loop regression documented.
3. Frozen data is freed when its last alias releases — deterministic, but RSS curves change; one element-alias pins its whole region (documented; `clone()` detaches).
4. `clone()` is now a guaranteed physical copy (it already was; this is now contractual).
5. `memory_stats`/region introspection report global refcounted regions.
6. **Not delivered, on purpose:** shared *mutable* state for closures. Thaw always copies. The Anneal closure-state wart (MEMORY.md) is about fluid sharing and remains; what this feature retires is the *read-only* sharing cost.

---

## 3. Judge-hole resolution index

Every distinct hole from the six verdicts, with its resolution:

| # | Hole (judge finding) | Resolution |
|---|---|---|
| H1 | Missing mutation guards: `OP_SET_INDEX`/`OP_SET_INDEX_GLOBAL` array/map/buffer prongs, `OP_SET_FIELD` map path, tree-walk index assign — **empirically verified** | **Stage 0** closes all of them as phase errors, before any sharing exists |
| H2 | Buffer native family (write_u8 etc.) zero phase checks; `realloc` on arena interior pointer | **Stage 0** adds crystal rejection to every mutating native across all three dispatchers; buffers only enter regions after Stage 3, by which point writes are rejected |
| H3 | Backend divergence (regvm key_phases holes vs stackvm rejection; tree-walk vs VM frozen-map behavior) | **Stage 0** reconciles: canonical semantics = *mutation of any crystal-phase value or crystal-tagged field/key is an error*; key_phases hole-mutation on a CRYSTAL-phase parent is removed (regvm) — freeze-except parents stay FLUID, which is the representation the predicate relies on |
| H4 | Stale `key_phases` after `freeze-except → refreeze` (verified: full freeze never normalizes key_phases) | **Stage 0**: `set_phase_recursive` clears/normalizes `key_phases` on full freeze (mirror of existing `field_phases` logic) |
| H5 | Compiled closures: `region_id` = upvalue count overload; `IS_REGION_PTR` misclassification | **Stage 1** migrates the count into closure-union padding (`uint32_t upvalue_count` after `has_variadic`, 7 bytes available — verified layout); ~10 stackvm + ~12 regvm + gc.c + `latc.c` serializer *and* loader sites; closures then carry `REGION_NONE` |
| H6 | Closures nested in frozen containers: `set_region_id_recursive` clobbers upvalue counts / walks `ObjUpvalue**` as `Env*`; mutable GC-owned upvalues inside "sealed" regions; tree-walk env non-atomic refcounts written inside arenas; thaw of frozen closure env_retains arena env | **Scoped out by the shareability scan (2.3)** — closure-containing values never regionize; legacy crystal fallback preserves today's behavior bit-for-bit. Honest cost: struct-with-methods doesn't share in v1 |
| H7 | LatRef/iterator/channel nested in regions: non-atomic refcounts cross-thread, O(1) page free can't run finalizers, gc stop-at-region UAF for region-held refs | **Scoped out by the shareability scan** — regions are pure data; O(1) free is sound; no finalizer walk needed |
| H8 | `set_region_id_recursive` missing `VAL_TUPLE` (verified) → free() of arena-interior pointers under bitwise extraction | Rewritten tagger in `value_freeze_to_region` covers tuple/map/set/enum/buffer/string; closures/refs unreachable (H6/H7) |
| H9 | Thaw taking the borrow fast path → FLUID handle over shared memory | `allow_share` parameter (no TLS); `value_thaw` uses `value_copy_out`; debug assert: no FLUID value ever carries a shared region_id |
| H10 | Steal patterns (`OP_GET_FIELD` etc.) write NIL into shared containers | Guarded per §2.5; element retain is sound because regions are self-contained (incl. tuples after H8) |
| H11 | `OP_MARK_FLUID`, `OP_SUBLIMATE`, borrow/crystallize phase restores create mutable views or break the phase-coupled predicate | Unshare-first rule (§2.5); **release keys on region tag only, borrow keys on tag+phase** — phase-anomalous handles can copy or release but never alias or leak |
| H12 | Partial thaw (`ROP_THAW_FIELD`) mutates field/key_phases on shared parents | Copy-on-partial-thaw (§2.5); per-backend since stackvm compiles non-ident thaw differently |
| H13 | `OP_FREEZE_FIELD/EXCEPT` VMs lack eval.c's crystal-parent guard; global path gets working copy via env_get and writes back franken-handles | **Stage 0** adds the guard (error on crystal parent, matching eval.c); fluid parents are never shared so the remaining path is safe |
| H14 | env_get contract change: ~50 callers, some get-modify-writeback | env_get's *callers* don't change behavior for fluid values (the only ones they legally modify); after Stage 0 + H13, every get-modify-writeback caller either operates on fluid values (never shared) or errors on crystal parents first. Belt-and-suspenders: Stage 2 includes a one-pass classification of all env_get callers in eval.c/stackvm.c/regvm.c/runtime.c/debugger.c with `value_unshare` inserted where a crystal working copy is legitimately mutated (history snapshots, etc. — most already route through deep_clone) |
| H15 | Pointer-range `IS_REGION_PTR` unsound on wasm32; memset-zero handle misclassified | Low-bit tag predicate (§2.2); zero fails the bit test |
| H16 | Refcount-balance audit: value_free no-op currently forgives unbalanced ownership; move sites (OP_SET_LOCAL_POP, OP_RETURN, frame-slot reuse) must not drift | Same-handle double-free stays forgiving (memset→0→no release). Moves are zero-retain/zero-release by definition. Debug rc-ledger (per-region retain/release counters, teardown assert) + full ASAN matrix + mprotect tripwire + **differential oracle build** (`LATTICE_FORCE_COPY=1` disables the borrow branches; semantics must be bit-identical) |
| H17 | GC-sweep destructor-less reclaim pins regions forever; deleting region_collect removes the mop | §2.6: region_collect retained as debug leak *detector*; eager-free VM discipline; documented residual |
| H18 | Census safepoint UAF (defer, builtin callbacks) | Moot — no census; refcount lifetime |
| H19 | `--no-regions` mode | Predicate never matches (freeze stays tag-flip there); feature transparently off; `value_detach` retained for that mode |
| H20 | Extension ABI (ext.c clones/frees LatValues) | Copy-out at the ext boundary (§2.4) |
| H21 | Freeze sites beyond the opcodes: ~10 bare `value_freeze` calls in eval.c (8572, 8637, 8702, 8825, 8851, 9154, 9210, 9256, fix-destructuring), runtime.c:129/591, struct-literal `fix` fields | **Stage 2/3 includes the explicit triage**: whole-binding freezes (`EXPR_FREEZE`, `OP_FREEZE/FREEZE_VAR`, fix bindings, anneal refreeze, mirror cascades) → `value_freeze_to_region` (with shareability fallback); per-field freezes and struct-literal `fix` fields → bare `value_freeze` (they live inside fluid parents — never shared, correct today). The triage table ships in the Stage 2 PR description |
| H22 | VAL_REF freeze-ordering semantics (set_phase_recursive crystallizes shared ref inners visible to outside aliases) | Ref-containing values are unshareable (H7) → legacy path → today's `value_freeze` then no regionization → behavior unchanged, ordering preserved |
| H23 | Sublimated members in frozen containers can't be copied at materialization | Unshareable scan excludes them → legacy fallback |
| H24 | `OP_ADD_LOCAL` / regvm concat region_id surgery sites outside the clone funnels | Explicit guards (§2.5) |
| H25 | Stats tests, `--stats` counters, MemoryStats semantics | Updated in-stage; acknowledged behavior change |
| H26 | Thin runtime `clat-run`, wasm_api, LSP/DAP link targets | Global registry lives in value.c/memory.c, linked by all targets; teardown at exit; `make wasm` and `make WINDOWS=1` added to the Stage gate (CI already builds clat-run) |

---

## 4. Staged implementation plan

Each stage is independently shippable, lands behind passing `make test`, `make test-tree-walk`, `make test-regvm`, `make asan`, and the new tests listed (written **first**, TDD). Stages 0–1 are pure pre-work that improve the language regardless of whether the feature ships.

### Stage 0 — Phase-guard hardening & semantics reconciliation (~1.5–2 weeks)
*The prerequisite the original designs all missed. Ships as a bug-fix release; file LAT tickets per hole.*

**Tests first** (cross-backend matrix, one .lat per case × 3 backends): frozen array index-assign rejected; frozen map index/dot-assign rejected; frozen buffer `write_*`/`push_*`/`fill`/`resize` rejected; frozen-global vs frozen-local parity; freeze-except → mutate exempt key OK / mutate frozen key rejected / **refreeze → mutate rejected** (key_phases normalization); freeze_field/except on already-crystal parent errors in VMs (parity with eval.c); regvm key_phases-hole policy on CRYSTAL-phase parents removed; mutating-method dispatch on crystal receiver rejected (sort-in-place, map.remove, etc.).

**Files:** `src/stackvm.c` (OP_SET_INDEX ~4615-4689, OP_SET_INDEX_GLOBAL ~6347-6430, OP_SET_INDEX_LOCAL, OP_SET_FIELD map path ~4826, freeze_field/except guards 7217-7396, builtin dispatcher), `src/regvm.c` (key_phases policy 3599-3611/5954-5962, buffer dispatch, ROP_FREEZE_FIELD guards), `src/eval.c` (index-assign lvalue path, method dispatch), `src/builtin_methods.c` (crystal rejection in every mutating method — currently **zero** checks), `src/value.c` (`set_phase_recursive` key_phases normalization).

**Retires:** the soundness-blocking premise gap. **Perf:** none. **Wart:** none yet — but fixes real silent-mutation bugs shipping today.

### Stage 1 — Closure upvalue_count migration (~3–4 days)
**Tests first:** closure-heavy suite passes; new C asserts that every closure value carries `region_id == REGION_NONE`; .latc round-trip (self-hosted compiler `latc.lat` → serialize → load → run) byte-identical output.

**Files:** `include/value.h` (`uint32_t upvalue_count` in closure union padding — zero size growth, verified layout), `src/stackvm.c` (10 sites: 4101/4153/4189/4883/4935/5103/5162/5398/5445/5601), `src/regvm.c` (~12 sites), `src/gc.c` (delete the region_id exception branches at 127-133/308-309, mark upvalues via the new field), `src/latc.c` (serializer ~720 + loader ~879-894), `src/value.c` (clone paths preserve the field; **audit**: the value_free no-op at `region_id != -1` currently shields closure frees — after migration closures hit the real free path; verify `value_free`'s VAL_CLOSURE arm and both VMs' inline frees handle bytecode closures correctly, per the "load-bearing no-op" judge finding).

**Retires:** the field collision blocking pointer-tagged region ids. `.latc` on-disk format unchanged.

### Stage 2 — Global refcounted regions + tree-walker sharing (~2 weeks)
**Tests first:** alias-then-thaw isolation (two bindings, thaw one, mutate, other unchanged — locals/globals/struct-fields-in-fluid-parents/nested); freeze idempotence (refreeze = same handle, O(1)); shareability fallback (freeze struct-with-closure → legacy crystal, semantics identical to v0.4); shared-crystal-inside-fluid-container clone/free rc balance; partial-thaw copy-on-write; anneal rebinding (old aliases see pre-anneal value); mirror-bond cascade onto shared/shareable/unshareable targets; rc-ledger teardown assert; mprotect tripwire suite; REPL persistence across lines; leak-detector reports (debug); differential oracle (`LATTICE_FORCE_COPY=1`) full-suite equivalence.

**Files:** `include/memory.h` + `src/memory.c` (atomic rc, retain/release, global registry, mprotect debug seal, leak detector hooks), `include/value.h` (predicate macros), `src/value.c` (`value_clone_impl(v, allow_share)`, `value_copy_out`, `value_unshare`, `value_freeze_to_region` with shareability scan + rewritten tagger, `value_thaw` → copy_out, `value_free` release branch, `value_detach` retain path, `set_phase_recursive` shared-handle assert), `src/eval.c` (freeze-site triage per H21, delete region_collect *freeing* / keep as debug detector, EXPR_THAW/ANNEAL/borrow/crystallize audit, env_get caller classification pass), `src/runtime.c` (cascade/bond/history paths), `src/env.c` (env_detach_values skip), `src/ext.c` (copy-out boundary), `tests/test_memory.c` + `tests/test_eval.c` (stats test rework).

**Perf wins land here (tree-walker):** crystal reads/passes/returns O(1); spawn env export of frozen data O(1); `env_get` of frozen globals O(1). **Wart partially retired:** spawn deep-clone-the-world for frozen data (tree-walker).

### Stage 3 — Stack VM (default backend) (~2 weeks; the long pole)
**Tests first:** all Stage-2 aliasing tests on the VM; steal-pattern guards (field/tuple/map/enum extraction from shared containers, OP_INVOKE self); MARK_FLUID/SUBLIMATE-on-shared unshare; fix-binding freeze benchmarks (regression gate); channel send/recv rc balance under threads (incl. teardown with buffered crystals, sender-death ordering); spawn alias outliving parent rebinding; ephemeral-arena → region force-copy (freeze of ephemeral-backed string); OP_ADD_LOCAL guard; select-arm handle transfer.

**Files:** `src/stackvm.c` — `value_clone_fast` borrow branch (:239), OP_FREEZE/FREEZE_VAR → `value_freeze_to_region` (6824, 7121), OP_THAW → copy-out thaw, OP_CLONE → copy_out, steal guards (4704-4760, invoke paths), OP_MARK_FLUID/OP_SUBLIMATE unshare, `stackvm_promote_value` skip-shared, channel-send method retain path (2240-2268), `stackvm_export_locals_to_env` (automatic via deep_clone), OP_ADD_LOCAL guard (~8180). `src/channel.c` — retain/enqueue path, recv/teardown release.

**Perf wins:** the headline numbers — channel send of crystal: 2 deep copies → 1 retain; spawn over frozen dataset: O(n)·workers → O(1)·workers; every read/arg-pass/iteration of frozen data O(1) on the default backend. **Warts retired:** "channels deep-copy everything"; "spawn isolation deep-clones whole environments" (for frozen state).

### Stage 4 — Register VM (~1 week)
**Tests first:** Stage-3 matrix on regvm; ROP_THAW_FIELD copy-on-partial-thaw; register-window move/free balance; upvalue-close path (rvm_clone at :3926 — borrow branch makes it correct *and* cheap); in-place concat guard.

**Files:** `src/regvm.c` — `rvm_clone` borrow branch (:387; `rvm_clone_or_borrow` inherits), ROP_FREEZE family → materialize, ROP_THAW/THAW_FIELD, ROP_CLONE, concat guard, channel/spawn paths.

### Stage 5 — Concurrency hardening (~1 week, overlaps 3–4)
Thread-stress tests (N senders/receivers churning shared crystals; spawn trees; abandoned channels with buffered crystals; thread-death ordering), TSAN run, retain-visibility documentation in `memory.h`, `make WINDOWS=1` + `make wasm` + `clat-run` build/test gates.

### Stage 6 — Perf validation, docs, rollout (~1 week)
Benchmark suite: freeze-cost regression (fix-heavy code, freeze-in-loop), read/spawn/channel wins, regionization size threshold tuning (scalars/short strings exempt). Docs: two-crystal-classes model, pinning + `clone()` escape hatch, what does NOT share (closures), `memory_stats` changes. Rollout: ship behind `LATTICE_SHARE_CRYSTALS=1` for one release with the differential oracle as the default-off safety net; flip default the following release; the oracle build mode is kept permanently for debugging.

---

## 5. Effort

**9–12 weeks** total for one engineer who knows this codebase — deliberately honoring the judges' consensus that the candidates' 4–6 week figures were ~2x optimistic. Stage 0 (~2w) and Stage 1 (~0.5w) are independently valuable pre-work; the sharing core is ~6w; hardening/perf ~2w. Budget at least a third for ASAN/TSAN/stress-found ownership bugs (LAT-430-class history). Per project convention, every stage closes with the full three-backend suite before its ticket closes.

## 6. Explicitly out of scope (v1), with the follow-up shape

1. **Closure sharing** — needs the closure-struct refactor (Stage 1 is its first step), an upvalue-immutability-at-freeze story, and atomic Env refcounts. ~2–3 weeks, separate design review.
2. **Shared mutable state / lazy COW thaw** — thaw stays an explicit O(size) copy; the language's isolation model is preserved on purpose.
3. **Cross-region dep edges** (avoid re-copying nested crystals on refreeze) — deferred; self-containment is what makes v1's refcounting trivially sound.
4. **Thread-confined nonatomic refcount optimization** — only if profiling shows contention; uncontended atomic add is ~1ns vs the deep clone it replaces.

The single most important sentence in this design, for every future patch: **shared region memory — including the phase tags stored inside it — is never written after seal; the only legal exits are `value_copy_out` (thaw/clone/unshare) and `crystal_region_release` (death), and any code that wants to write a phase tag or a byte through a handle must `value_unshare` it first.**