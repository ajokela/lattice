# Freeze-Time Data-Structure Optimization for Lattice Maps — Final Design

**Status:** Approved for implementation. Chosen design: **perfect-hash-flat ("Crystallized Map Index", CMI)**, as revised by adversarial review and validated by a working prototype (worktree commit `04d381d`, stack-VM-only, all 1849 tests green on all three backends, ASan/UBSan clean).

All file/line references verified against `/Users/alexjokela/projects/lattice` at `main` (`2b87d77`) and the prototype worktree `/Users/alexjokela/projects/lattice/.claude/worktrees/wf_01f44602-90f-18`.

---

## 1. What we are building, and the honest performance story

When `freeze()` fully freezes a map of >= 16 keys, the runtime rebuilds it into a single contiguous block: a dense, **order-preserving** copy of the occupied entries, a CHD minimal perfect hash for lookups, and packed key bytes. The block is the map's `entries[]` array — a legal, 100%-occupied open-addressing table in the source map's original slot-scan order — so every existing iteration/display/equality/JSON site works unchanged and gets faster.

### 1.1 Measured results (prototype, stack VM, -O3, Apple Silicon, 3-run medians, frozen ns/op, checksums frozen==fluid everywhere)

| scenario | size | baseline | prototype | change |
|---|---|---|---|---|
| iter | 65536 | 729.5 | 113.2 | **-84% (6.4x)** |
| iter | 1000 | 187.9 | 94.5 | **-50%** |
| iter | 64 | 156.5 | 98.0 | **-37%** |
| has_miss | 1000 | 138.0 | 102.7 | **-26%** |
| has_miss | 65536 | 148.0 | 121.0 | **-18%** |
| lookup_miss | 1000 | 183.3 | 149.7 | **-18%** |
| lookup_miss | 65536 | 190.7 | 169.4 | **-11%** |
| lookup_hit | 64 | 134.0 | 115.7 | **-14%** |
| lookup_hit | 1000 | 137.7 | 129.0 | -6% |
| lookup_hit | 65536 | 176.4 | 172.0 | -2.5% (parity) |
| size 8 (below threshold) | — | — | — | no-op by design |
| fluid columns | all | — | — | unchanged within noise |
| freeze() of 64k map | — | 125 ms | 84 ms | **1.5x faster** |

### 1.2 The original prediction was wrong about hits — we adjust the headline

The judged design predicted **-15..-20% on 64k lookup hits**. The prototype shows that does **not** materialize: at the raw hashmap level the CHD hit path is ~50% *slower* than the sparse probe (31.5 vs 21.1 ns), because it is a serial dependency chain (mix → disp load → mix → packed load → entry → strcmp) versus the sparse table's average ~1.05 probes at 50% load. The VM's ~100-130 ns interpreter floor masks this, leaving 64k hits at parity instead of clearly faster.

**The feature is still clearly worth shipping, with the wins re-attributed:**

1. **Iteration** is the headline: up to **6.4x at 64k**, -37..-50% at moderate sizes. This attacks exactly the worst-scaling baseline scenario (the dense block plus block-copy clones eliminate both the sparse scan and the sparse rebuild that precedes every for-in).
2. **Misses** win consistently (-11..-26%): the 32-bit hash filter in the packed position table kills probe chains and almost all strcmps.
3. **Freeze and clone get cheaper**, not more expensive, at scale: `OP_FREEZE_VAR`'s two map clones (`src/stackvm.c:7121-7123`: `value_deep_clone(&frozen)` for the result, `stackvm_write_back` for the slot) become block memcpys; 64k freeze drops 125 ms → 84 ms despite paying the ~28 ms CHD build. The judges' break-even objection ("16 is 60x too low") dissolved empirically: freeze of 64/1000-key maps is unchanged at ms resolution, the 2000x small-map freeze/thaw churn benchmark is unchanged, and large freezes are net faster.
4. **Hits** are at parity at 64k, modestly positive at small/medium sizes (-6..-14%). The pre-existing ~7% frozen-slower-than-fluid regression at 64k is mostly erased (1.067 → 1.051 frozen/fluid).
5. **Memory** at 64k: ~7.1 MB block vs 12.6 MB sparse table (cap=2n × 96 B) — on the heap paths where the old table is freed. (Tree-walker arena path: see §6.)

**Why keep the perfect hash at all, given hit parity?** Because the misses and the memory halving come from it (exactly one probe position, load factor 1.0), and the build cost is already paid for by the clone savings. A considered alternative — dense entries + a conventional 50%-load (hash32|index) index — would recover a few ns on hits but give back the miss win and half the index memory; it remains a documented fallback if future hardware data says otherwise.

---

## 2. Chosen layout (as built and measured)

From the prototype's `include/ds/hashmap.h`:

```c
typedef struct CrystalMapHeader {
    uint64_t n;          /* live key count */
    uint64_t nbuckets;   /* CHD buckets (~n/4) */
    uint64_t seed;       /* hash-mix seed */
    uint64_t refcount;   /* RESERVED for crystal-by-ref aliasing (unused, init 1) */
    uint64_t total_size; /* whole block size — enables memcpy clone/alias */
    uint64_t off_entries, off_packed, off_disp, off_keys;
} CrystalMapHeader;

typedef struct {
    LatMapEntry *entries;
    size_t cap, count, live, value_size;
    CrystalMapHeader *cmi;   /* NULL = normal open-addressing layout */
} LatMap;
```

Block contents (offsets from block start, one raw `malloc`):

- `dense[n]` — `LatMapEntry` array in **original slot-scan order**, all `MAP_OCCUPIED`; `entry.value` points at its own inline `_ibuf` (the 72-byte `LAT_MAP_INLINE_MAX` contract is preserved); `entry.key` points into the packed key bytes.
- `packed[n]` — `uint64_t` per perfect-hash position: `(hash32 << 32) | dense_index`. **This is a deliberate refinement over the judged design's separate `key_hash[]` + `idx[]` arrays:** one load yields both the miss filter and the entry index, removing a dependent cache miss from the lookup chain. Correctness never depends on hash width — strcmp always verifies.
- `disp[nbuckets]` — `uint16_t` CHD displacements, lambda=4, up to 3 seed retries, **sparse-layout fallback on build failure** (correct, just unoptimized).
- packed NUL-terminated key bytes (deliberately **not** interned — keeps the block self-contained for future refcounted aliasing; the intern table is global, mutexed, and never freed).

After crystallization: `m->entries = block + off_entries; cap = count = live = n; m->cmi = header`.

**Hash-mix constraint discovered empirically (not in any judged design):** a purely linear single-multiply displacement mix makes CHD construction fail at load factor 1.0 — all keys of a bucket shift as a rigid block as `d` varies, so collisions are unresolvable. The mix must include an xor between two multiplies. This is load-bearing; it is documented in the prototype's `hashmap.c` and must not be "optimized" away.

**Dispatch** lives entirely in `src/ds/hashmap.c`: a `m->cmi` NULL-check prepended to `lat_map_get`, `lat_map_get_prehashed`, `lat_map_contains`, `lat_map_set`, `lat_map_remove`, `lat_map_free`. Measured cost to fluid maps: zero (fluid columns unchanged within noise).

---

## 3. Resolution of every judge-found hole

The three adversarial verdicts on perfect-hash-flat found 9 distinct holes (and the other two designs' verdicts found the same classes of bugs, confirming they are systemic). Each is resolved, and all resolutions are **already implemented and validated in the prototype** unless marked otherwise.

**H1 — `value_clone_fast` strips the layout (judges' #1, "fatal").** Confirmed real: the stack VM's `value_clone_fast` VAL_MAP case (`src/stackvm.c:331-355`) rebuilds a fresh sparse table via `lat_map_set` and is the actual workhorse for `OP_GET_LOCAL`/`OP_SET_LOCAL`/`OP_GET_GLOBAL`/call arguments — including the write-back one instruction after `freeze(m)`. **Resolution:** `value_clone_fast` gets a CMI block-copy branch (memcpy `total_size`, rebase `entry.key` by delta, repoint `entry.value` at own `_ibuf`, then `value_clone_fast` each entry's nested value in place). This was indeed load-bearing: most of the measured iteration win comes from this branch, since for-in clones the whole map before key-array conversion. Note the **entry self-pointer fixup** (`entry.value = &own _ibuf`) is a stated invariant of every block copy — the shape-sharing verdict flagged its omission as the likeliest silent-UAF bug; it is explicit in `lat_map_cmi_clone_block`.

**H2 — GC sweeps / fluid-heap corruption of the block.** Confirmed: `gc_mark_value`'s VAL_MAP case (`src/eval.c:275-284`) marks the `LatMap` struct and entry values but **never** `m->entries` — safe today only because entries are raw `calloc` (hashmap.c:19-28), invisible to the fluid tracker. **Resolution:** the CMI block is always **raw malloc**, never `lat_alloc_routed`; the `value_deep_clone` block-copy branch is guarded `!g_arena && !g_heap` (`src/value.c` in prototype, line ~635) so no tracked-heap path ever produces a block. `lat_map_free`'s cmi branch does one raw `free(cmi)` and skips per-key frees, matching the raw-allocation discipline.

**H3 — thaw contradiction (clone preserves cmi, thaw must not).** Confirmed: `value_thaw` (`src/value.c:576-589`) is deep-clone + fluidify, and a cmi-preserving clone would yield a crystallized-but-fluid map. **Resolution:** `value_thaw` explicitly calls `value_decrystallize` on the clone before fluidifying. `lat_map_decrystallize` rebuilds the standard table by inserting dense entries in order into a fresh `lat_map_new` table (INITIAL_CAP=16, deterministic growth) — since dense order equals today's slot-scan order, the post-thaw table is **bit-identical** to today's thaw output. Invariant enforced: `cmi != NULL ⇒ phase == VTAG_CRYSTAL`, asserted in debug builds.

**H4 — `lat_map_set` rehash landmine.** Confirmed: the load-factor check runs first in `lat_map_set` (`src/ds/hashmap.c:93-96`) and is always true on a cap==count CMI map; a rehash would `free()` the block-interior entries pointer. **Resolution:** the cmi dispatch in `lat_map_set` runs **before** the load-factor check (existing-key writes do an in-place `_ibuf` memcpy; new-key writes decrystallize first). Same ordering discipline in `lat_map_remove` and `lat_map_free` (cmi branch precedes the per-key free loop). A debug-build assert fires on any decrystallize-via-backstop so phase-check gaps surface in CI instead of hiding as perf cliffs.

**H5 — partial-freeze guards (FREEZE_FIELD reachable on crystal maps; FREEZE_EXCEPT unguarded).** Confirmed: stack VM `OP_FREEZE_FIELD` performs no parent-phase check, so `freeze(m); freeze_field(m,"k")` is a live path. **Resolution:** eligibility requires `key_phases == NULL`, `region_id == REGION_NONE`, `value_size == sizeof(LatValue)`, `live >= CMI_MIN_KEYS (16)`, phase CRYSTAL; **both** `OP_FREEZE_FIELD` and `OP_FREEZE_EXCEPT` map branches decrystallize before attaching `key_phases` (per-backend, each rollout stage). Partially frozen maps are never crystallized — period. Covered by dedicated tests (freeze_field-after-freeze at 40 and 65536 keys).

**H6 — arena double-residency / arena decrystallize leak (tree-walker).** Real, and **not solved by the prototype** — the prototype scopes the tree-walker out entirely (`!g_arena && !g_heap` guard; cmi always NULL there). Resolution is Stage 4's whole job; see §6. Until Stage 4 lands, the tree-walker is simply unoptimized and 100% unchanged, which is correct.

**H7 — 32-bit/WASM hash width.** Confirmed: `fnv1a` returns `size_t` (`src/ds/hashmap.c:10-17`), 32 bits on wasm32, degrading both the filter and CHD build quality (birthday collisions at 64k make build failure likely). **Resolution (Stage 3 item):** the CMI build and lookup use a dedicated explicit-`uint64_t` FNV-1a regardless of `size_t` width; `lat_map_get_prehashed` callers (env.c) are unaffected because the cmi path recomputes its own hash. The build-failure fallback to sparse remains as defense in depth. The mulhi `fastrange` ships with a modulo fallback under `#if` for WASM/mingw-without-`__int128`.

**H8 — unfair baseline (`%` vs power-of-two mask).** Valid: every sparse probe pays one-two 64-bit divisions (`hashmap.c` probe loop) despite cap always being a power of two. **Resolution:** Stage 0 lands the `& (cap-1)` mask fix for *all* maps first, and all CMI numbers are re-measured against that corrected baseline. Expectation: lookup deltas shrink a few ns; iteration, miss-filter, and clone wins (the actual headline) are unaffected since they don't come from division elimination.

**H9 — internal-ABI footgun: `sizeof(LatMap)` grows by 8.** Empirically confirmed during prototyping: one stale object after a stash/build/pop produced a deterministic SIGBUS in an unrelated test. **Resolution:** no external ABI exposure (extensions use the `lat_ext_*` API and receive `LatValue*` only — verified against `extensions/`), but the implementation PR notes "clean rebuild required", and the Makefile's header-dependency tracking (landed in `571e1be`) makes incremental builds safe going forward. Constructor audit completed: `cmi=NULL` set in `lat_map_new`, both arena clone paths in `value.c`, and `env_clone_arena` in `env.c`.

**Cross-cutting findings from the other designs' verdicts that apply here:**

- **Channels:** `value_detach` deep-clones with `g_heap`/`g_arena` both NULL — which is exactly the condition under which the block-copy branch is active. **Frozen maps sent over channels keep their crystallized layout.** (The sorted-adaptive verdict flagged silent stripping; our guard polarity makes detach the preserving case. Add an explicit cross-thread send test.)
- **Globals:** `OP_GET_GLOBAL` clones the whole map per access (`value_clone_fast`); with the block-copy branch that clone becomes a memcpy — much faster, but still O(n) per read. Eliminating the per-access container clone is explicitly the sibling crystal-by-ref feature's job, not this one's.
- **Concurrency:** crystallization happens only inside freeze operations on values the executing frame owns (pass-by-value); no aliased `LatMap*` exists across spawn tasks today. The VAL_REF thaw path (`src/value.c:577-585`) deep-clones, breaking sharing. Rule recorded for the sibling: **crystallize strictly before any aliasing is introduced**; never rebuild a possibly-aliased map's representation in place.
- **`.latc` serialization:** map constants don't exist in chunk constant pools today (`src/latc.c`); a comment in `hashmap.h` instructs any future serializer to walk logical entries (which the dense layout supports via the ordinary `entries[]` scan) and never write `cmi` raw.

---

## 4. Iteration order, equality, display — the semantics proof

Frozen-map observable semantics are preserved **by construction**, verified three independent times by judges via grep:

- `dense[]` is laid out in the source table's slot-scan order at the instant of freeze — exactly the sequence `lat_map_iter`, `iter_from_map` (`src/iterator.c:58-74`), `stackvm_iter_convert_to_array` (`src/stackvm.c:2923-2939`), `builtin_map_keys/values/entries` (`src/builtin_methods.c:1032-1082`), eval.c for-loops, `value_display`, `value_hash_key`, and the json/yaml/toml writers would have produced. All of them are pure linear `MAP_OCCUPIED` scans; no probe-position logic exists outside `src/ds/hashmap.c`.
- A frozen map can never rehash, so the captured order is stable for the crystal's lifetime.
- `value_eq` is content-based (iterate one map, `lat_map_get` the other), so mixed crystallized/sparse equality is exact.
- thaw/clone-to-mutable reinsert in dense order == today's slot-scan reinsertion order, so downstream tables are bit-identical to today's (§3 H3).
- Benchmark harness verifies frozen==fluid checksums for every scenario at every size; the prototype passed all of them. New order-pinning tests are added in Stage 1 since the existing suite deliberately doesn't pin order.

---

## 5. Partial freeze, thaw, clone — the full lifecycle

| transition | behavior |
|---|---|
| `freeze(m)`, eligible | crystallize after `value_freeze`; recursive `value_crystallize` also catches maps nested in arrays/structs/tuples/enums |
| `freeze(m)`, ineligible (small, key_phases, arena, wrong value_size) | no-op; sparse layout kept; semantics identical |
| `freeze_field` / `freeze_except` on fluid map | never crystallizes (attaches `key_phases`, which permanently fails eligibility — conservative, correct) |
| `freeze_field` / `freeze_except` on crystallized map | handler decrystallizes first, then proceeds exactly as today |
| `thaw(m)` | deep-clone → explicit decrystallize → fluidify; output table bit-identical to today's |
| `clone()` of crystallized map | block memcpy + key-pointer rebase + `_ibuf` repoint + per-entry nested deep-clone; **preserves** the optimization and is faster than today's clone |
| mutation reaching a crystallized map (phase-check gap) | `lat_map_set`/`lat_map_remove` backstop decrystallizes (heap maps) — correct-but-unoptimized, plus debug assert; on region-tagged maps this is a **hard error** in debug and an arena-routed rebuild in release (§6) |
| channel send | `value_detach` preserves the block (§3) |
| free | `lat_map_free`/`value_free` cmi branch: free nested entry values, then one raw `free(cmi)`; per-key free loop skipped. Highest-severity footgun; concentrated in two files, ASan-gated |

Future improvement (explicitly deferred, ticketed): when a full `freeze()` finds `key_phases` present but every entry is `VTAG_CRYSTAL`, drop `key_phases` and crystallize.

---

## 6. Tree-walker / arena story (Stage 4 — the one unsolved part, designed here)

The prototype excludes the tree-walker because the naive hook ("crystallize after `value_deep_clone` inside `freeze_to_region`", `src/eval.c:501-516`) was correctly shredded by all three verdicts: it strands the 12.6 MB sparse arena clone next to the 7.1 MB block (arenas never free interiors), and decrystallize-to-heap on a region-tagged value leaks and violates the dual-heap invariant.

**Stage 4 design:**

1. **Build dense directly, not additionally.** The arena-mode VAL_MAP branch of `value_deep_clone` (`src/value.c:406-429`) is **replaced** for eligible maps: it builds the dense block straight from the fluid source into the arena via `arena_alloc` (one allocation, 64-byte aligned), skipping the sparse copy entirely — including tombstones/empties the current code faithfully clones. This is a rewrite of that branch, not a one-line hook; the effort estimate accounts for it. Ineligible maps take the existing layout-preserving sparse path unchanged.
2. **GC:** the block is arena memory inside the value's CrystalRegion before `set_region_id_recursive` runs; `gc_mark_value`'s crystal early-return (`src/eval.c:230-236`) and `assert_dual_heap_invariant` (`eval.c:313-373`, which only requires absence from the fluid list) hold without modification.
3. **Decrystallize on arena maps allocates into the same region.** When eval.c's freeze_field/thaw_field/freeze_except handlers (or the set/remove backstop) must restructure an arena-resident CMI map, the replacement sparse table is allocated via the **owning region's arena** (regions are growable page lists; verify `arena_alloc` page-extension during Stage 4 kickoff — it is the standard allocation path so this is expected to hold). The dead block stays as arena waste on a rare path; no `free()` of arena interiors, no heap pointers under a region-tagged value, no leak — region teardown reclaims everything. This converts the judges' "leak generator" into bounded, teardown-reclaimed waste.
4. **No double work:** eval.c's `value_freeze` → `freeze_to_region` sequence does not get a heap-side crystallize hook (the `!g_arena && !g_heap` guard already prevents it); crystallization on the tree-walker happens exactly once, inside the arena clone.

If Stage 4's measured complexity-to-benefit ratio disappoints (the tree-walker is the legacy `--tree-walk` backend), the acceptable fallback is to ship Stages 0-3 and leave the tree-walker permanently sparse — semantics are identical either way.

---

## 7. Composition with the sibling "crystal values shared by reference" effort

Choices made deliberately, in priority order, to keep that door open:

1. **Refcount headroom:** `CrystalMapHeader.refcount` (u64, init 1, unused in v1) is reserved at a fixed offset. Aliasing a crystallized map later is "bump refcount, share block pointer"; clone-by-alias replaces today's block memcpy with no layout change.
2. **Single-allocation, rebasable block:** all interior pointers (`entry.key`, `entry.value`) are block-relative-rebasable, and `total_size` enables whole-block operations. A future aliased block is one ownership unit with stable pointers.
3. **Keys are not interned** — the block is self-contained, so a refcounted block's lifetime never entangles with the global never-freed intern table. A flags bit is reserved if the sibling prefers an interned-key variant.
4. **The division of labor is explicit:** the ~100-130 ns interpreter + value-clone floor visible in every benchmark row (and the O(n) container clone on `OP_GET_GLOBAL`) is exactly what crystal-by-ref attacks; CMI deliberately does not touch the post-lookup `value_deep_clone` at `stackvm.c:4524` / `regvm.c:3556`. Together: alias the frozen map (no container clone) + CMI layout (fast probe, dense iteration) is the end state; each ships independently.
5. **Ordering rule for the sibling (recorded in §3):** crystallization must complete before any alias to the map exists; representation swaps on aliased values are forbidden. The existing `value_thaw` VAL_REF branch ("thaw breaks sharing", `src/value.c:577-585`) already establishes the clone-on-unshare convention the sibling extends.

---

## 8. Staged implementation plan

Tests first in every stage; full gate everywhere = `make test` + `make test-tree-walk` + `make test-regvm` + `make asan`, plus `make wasm` and `make WINDOWS=1` builds from Stage 1 on. Benchmarks: `benchmarks/frozen_map_read.lat` + `scripts/bench_frozen_map_read.sh` (recreated in the prototype) become permanent.

**Stage 0 — Honest baseline (0.5-1 day).**
*Tests first:* none new; existing suite must stay green.
*Changes:* power-of-two mask (`& (cap-1)`) replaces `%` in `src/ds/hashmap.c` probe loops; merge `benchmarks/frozen_map_read.lat` + bench script to main; re-record the full frozen/fluid grid as the official baseline.
*Improves:* every scenario, fluid **and** frozen, all three backends — and keeps CMI's subsequent claims honest.
*Files:* `src/ds/hashmap.c`, `benchmarks/`, `scripts/`.

**Stage 1 — CMI core in hashmap.c (2-3 days; mostly cherry-pick from `04d381d`).**
*Tests first:* `tests/test_ds.c` units — crystallize/decrystallize round-trip bit-identity, iteration-order pinning, n = 0/1/15/16/17/64k boundaries, prefixy/adversarial keys, build-failure fallback, set/remove backstop ordering (rehash landmine), free-path ASan coverage.
*Changes:* `CrystalMapHeader`, `lat_map_crystallize` / `lat_map_decrystallize` / `lat_map_cmi_clone_block`, explicit-u64 FNV for CMI (H7), dispatch in get/get_prehashed/contains/set/remove/free with set-before-load-factor-check ordering, mulhi + fallback.
*Improves:* nothing user-visible yet (no callers); establishes the invariant surface.
*Files:* `include/ds/hashmap.h`, `src/ds/hashmap.c`, `tests/test_ds.c`.

**Stage 2 — value.c + stack VM (default backend) (3-4 days; the prototype's proven scope).**
*Tests first:* `.lat` integration tests at 40 and 65536 keys: lookup/iter/keys-values-entries/json-stringify/clone/arg-passing/global-read/thaw/refreeze/freeze_field-after-freeze/freeze_except/channel-send-across-scope; order-pinning; checksum frozen==fluid.
*Changes:* `value_crystallize`/`value_decrystallize` recursion; `value_thaw` explicit decrystallize; `value_deep_clone` block-copy branch guarded `!g_arena && !g_heap`; constructor audit (`lat_map_new`, both arena clone paths, `env_clone_arena`); stack VM: `value_clone_fast` block-copy branch (H1 — load-bearing), hooks after `value_freeze` in `OP_FREEZE`/`OP_FREEZE_VAR` + cascade, decrystallize guards in `OP_FREEZE_FIELD`/`OP_FREEZE_EXCEPT`.
*Improves (measured):* iter 64k -84%, iter 1k -50%, misses -11..-26%, hits -2.5..-14%, 64k freeze 1.5x faster — on the default backend.
*Files:* `src/value.c`, `src/stackvm.c`, `src/env.c`, `tests/`.

**Stage 3 — register VM (1-2 days).**
*Tests first:* Stage 2 integration matrix run under `make test-regvm`; regvm-specific freeze/local/env-sync tests.
*Changes:* hooks at `ROP_FREEZE`/`ROP_FREEZE_VAR` (`src/regvm.c` ~4856-4935), decrystallize guards at `FREEZE_FIELD` (~6606) and `FREEZE_EXCEPT` (~6671-6768), decrystallize on regvm thaw paths. Cheap because `rvm_clone` already routes map clones through the now-CMI-aware `value_deep_clone`, and `ROP_GETINDEX` already funnels through `lat_map_get`. Verify wasm32 behavior of the u64 hash here (wasm builds the bytecode VMs).
*Improves:* the same grid on `make bench-regvm`.
*Files:* `src/regvm.c`, `tests/`.

**Stage 4 — tree-walker + arena (3-4 days; §6).**
*Tests first:* arena freeze of 64k map (region byte accounting asserted: dense block only, no sparse residue), freeze_field/thaw_field on arena CMI map (same-region decrystallize, no leak under ASan + region teardown), `eval_arena_freeze_map`-style round trips, dual-heap invariant suite, no_regions mode.
*Changes:* rewrite `value_deep_clone`'s arena VAL_MAP branch to build dense-direct for eligible maps; region-aware decrystallize (same-region arena allocation); eval.c partial-phase handler guards; verify arena page-extension and 64-byte alignment in `src/memory.c`.
*Improves:* tree-walker frozen reads + erases the arena-clone memory overhead (12.6 → ~7.1 MB at 64k) instead of doubling it.
*Files:* `src/value.c`, `src/eval.c`, `src/memory.c` (verification), `tests/test_eval.c`, `tests/test_memory.c`.

**Stage 5 — polish and ticketed follow-ups (1 day + backlog).**
Sets (`VAL_SET` shares `LatMap` — mechanical extension); `key_phases`-all-crystal promotion on refreeze; `pressure("read_heavy")` lowering `CMI_MIN_KEYS`; Eytzinger/alternative-index experiment only if post-mask-fix data justifies; release notes (no observable semantic changes; freeze of large maps faster; clean-rebuild note for embedders).

**Total: 11-15 working days** (the judges' 15-20 estimate was for fixing the holes from scratch; the prototype has already implemented and validated Stages 1-2's hard parts). Per project policy, every stage closes only after all three backend suites pass, and no commit carries AI attribution.

---

## 9. Risks that remain open

1. **Free-path ownership** (block-interior keys vs per-key frees) is the one real footgun; it is concentrated in `hashmap.c` + `value.c`, behind ASan in every stage gate.
2. **Stage 4 arena assumptions** (growable pages, alignment) are stated-but-unverified until its kickoff; the fallback (tree-walker stays sparse) is semantically free.
3. **Adversarial keys** can force CHD build failure (engineered 64-bit FNV collisions) — degrades to sparse, perf-only, documented.
4. **Post-mask-fix deltas** must be re-measured (Stage 0); iteration/miss/clone wins are structurally independent of the mask fix, but the published numbers must come from the corrected baseline.