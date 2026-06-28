# Lattice C Interpreter/VM — Adversarial Security Audit: Final Report

## 1. Executive Summary

This audit of the Lattice C interpreter/VM (~84k LOC) produced **45 confirmed findings**, each independently validated by 3 adversarial refuters: **4 Critical, 17 High, 16 Medium, 8 Low**. The overall risk posture is **serious but well-bounded by threat model**. Two structural defect classes dominate the high-severity tier: (1) **8-bit operand truncation of constant/sub-chunk indices** across all three code generators (`stackcompiler.c`, `regcompiler.c`) — when a chunk exceeds 256 constants, field/method/struct/enum names and concurrency sub-chunk pointers silently alias unrelated constants, yielding type-confusion and wild-pointer execution **from ordinary (if large) Lattice source**; and (2) **incomplete bytecode verifiers** (`chunk_verify`, `regchunk_verify`) that omit stack-balance, local-slot, and register-window checks, defeating the explicit trust boundary for untrusted `.latc`/`.rlatc`. The 4 critical issues add peer/attacker-controlled heap overflows in the network read path (`net.c`, `tls.c`) and the concurrency-sub-chunk truncation in the default backend. The tree-walker (non-default `--tree-walk`) carries several GC use-after-free/double-free defects; the register VM (non-default `--regvm`) carries type-confusion crashes. Medium/Low findings are predominantly resource leaks, unbounded-recursion DoS, and numeric undefined behavior. Most criticals require either a >256-constant program, a malicious peer, or an untrusted bytecode file — none are remote-without-cooperation in the default configuration, but several are trivially reachable from a single source file.

---

## 2. Findings by Severity

### CRITICAL

---

#### C-1 — Concurrency sub-chunk index truncated to 8 bits → VM executes an arbitrary constant as a `Chunk*`
- **Location:** `src/stackcompiler.c:2100, 2134, 2150, 2156-2158, 2186-2198`; consumed at `src/stackvm.c:7940, 7956, 7972` (OP_SCOPE) and `:8039` (OP_SELECT)
- **Category:** type-confusion
- **Mechanism:** `OP_SCOPE`/`OP_SPAWN`/`OP_SELECT` store their pre-compiled sub-chunks via `add_chunk_constant` (a `VAL_CLOSURE` that `chunk_add_constant` never deduplicates, `src/chunk.c:109-142`), so the returned index grows monotonically past 255. The emit sites cast it with a bare `(uint8_t)` — no 16-bit form and no overflow guard, unlike `emit_constant_idx` (`stackcompiler.c:104-114`). The VM reads the truncated index and does `(Chunk*)constants[idx].as.closure.native_fn` with no bounds/type check.
- **Trigger:** A top-level chunk with >256 constants followed by a concurrency form, e.g. `let a0=1000; … let a299=1299; scope { spawn { print("x") } }`. Empirically: spawn closure was real index 500, emitted operand byte 244; lands on a non-closure constant → SIGSEGV (exit 139); ASan reports `member access within null pointer of type 'Chunk'` in `stackvm_spawn_thread_fn → stackvm_run`.
- **Consequence:** The VM reinterprets a constant's union bits (attacker-steerable integers/string pointers) as an executable `Chunk*` — deterministic crash plus a control-flow-integrity violation in the **default** backend.
- **Suggested fix:** Add 16-bit operand forms for the OP_SCOPE/OP_SELECT/OP_SPAWN sub-chunk indices, or add a compile-time `compile_error` mirroring `emit_constant_idx`'s `>255` guard. As defense-in-depth, run `chunk_verify` on freshly compiled chunks, not just deserialized `.latc`.

---

#### C-2 — Field/method/struct/enum-name constant indices truncated to 8 bits → wild-pointer read in regvm `GETFIELD`/`INVOKE`
- **Location:** `src/regcompiler.c:1025` (and `1474, 1500, 1537, 2019, 2038, 2060` for INVOKE/INVOKE_LOCAL/INVOKE_GLOBAL, plus NEWSTRUCT/NEWENUM/FREEZE_VAR/THAW_VAR/SUBLIMATE_VAR); consumed at `src/regvm.c:3609-3614`
- **Category:** type-confusion
- **Mechanism:** `add_constant` returns a 16-bit index (`REGVM_CONST_MAX=65536`) but `ROP_GETFIELD` packs the field-name key into the 8-bit `C` operand via `(uint8_t)field_ki`. Distinct large ints are not collapsed by `regchunk_add_constant`, so >256 constants accumulate. The regvm reads `constants[c].as.str_val` and `strcmp`s it; a truncated index landing on an int constant reinterprets the int64 bytes as a `char*`.
- **Trigger:** `clat --regvm` with `struct S { field: int }`, ~300 distinct big-int constants, then `let s = S{field:5}; print(s.field)`. ASan: SEGV READ in `strcmp` at `regvm.c:3614`, `x1=0xf426d` (=1000045, the aliased int constant).
- **Consequence:** Deterministic wild-pointer read / arbitrary-address-read primitive whose target equals a program-controlled constant. No verifier runs on in-memory regvm chunks (`regchunk_verify` is only invoked from the `.rlatc` path, `latc.c:1002`).
- **Suggested fix:** Emit a 16-bit operand variant for these name-key opcodes, or `rc_error` when the constant index exceeds 255 (reuse the existing `REGVM_CONST_MAX` guard pattern with a tighter bound for 8-bit operands).

---

#### C-3 — `net_tcp_read_bytes`: `malloc(count+1)` integer overflow → peer-controlled heap buffer overflow
- **Location:** `src/net.c:215-236`; bindings at `src/eval.c:3839` and `src/runtime.c:1097`
- **Category:** int-overflow
- **Mechanism:** Both bindings do `(size_t)args[1].as.int_val` (`int_val` is `int64_t`) with only a `VAL_INT` type check — no sign/range validation. `n = -1` → `count = SIZE_MAX` → `malloc(count + 1) == malloc(0)`, which returns a non-NULL minimal chunk, passing the `!buf` guard. The `recv` loop then writes peer-controlled bytes from offset 0 into the ~0-byte allocation; `buf[total] = '\0'` is an additional OOB write.
- **Trigger:** `let fd = tcp_connect(host, port); tcp_write(fd, req); let s = tcp_read_bytes(fd, -1)`. (`-1` is the natural "read all remaining" idiom.)
- **Consequence:** Heap buffer overflow with attacker-controlled length and contents → heap corruption / potential RCE.
- **Suggested fix:** In the bindings (or at the top of `net_tcp_read_bytes`), reject `count < 0` and cap to a sane maximum before the cast: `if (args[1].as.int_val < 0 || args[1].as.int_val > MAX_READ) return error(...)`.

---

#### C-4 — `net_tls_read_bytes`: `malloc(count+1)` integer overflow → heap buffer overflow (Schannel; 1-byte OOB on OpenSSL)
- **Location:** `src/tls.c:127-148` (OpenSSL) and `src/tls.c:538` (Schannel); bindings at `src/eval.c:3966` / `src/runtime.c:1177`
- **Category:** int-overflow
- **Mechanism:** Same unchecked `(size_t)args[1].as.int_val` → `malloc(0)`. On the **Schannel/Windows** build (`tls.c:537-573`) the clamp `take = (total+clen > count) ? count-total : clen` is defeated when `count==SIZE_MAX`, so `memcpy(result+total, chunk, clen)` copies up to 64KB of TLS-peer plaintext into the 0-byte buffer — a genuine server-controlled heap overflow. On modern **OpenSSL**, `SSL_read` rejects the negative `(int)(count-total) == -1` length, so the loop breaks and only `buf[0]='\0'` is an OOB 1-byte null write (benign on glibc/macOS; worse on EOL OpenSSL 1.0.x).
- **Trigger:** `tls_read_bytes(fd, -1)` over a connection to a malicious HTTPS endpoint.
- **Consequence:** Server-controlled heap overflow on the Windows build (shipped); OOB null write elsewhere.
- **Suggested fix:** Range-check `count` (reject negative, cap to a sane max) in the bindings and in `net_tls_read_bytes` before `malloc(count+1)` at both `tls.c:133` and `tls.c:538`.

---

### HIGH

---

#### H-5 — STMT_ASSIGN: RHS value un-rooted across `resolve_lvalue`'s index eval → GC-swept before store (UAF + double-free)
- **Location:** `src/eval.c:10603-10895`
- **Category:** UAF (tree-walker, `--tree-walk`)
- **Mechanism:** `valr.value` (the RHS) is a bare C local that is never `GC_PUSH`'d. For `arr[idx()] = [..]`, the generic path reaches `resolve_lvalue` (`eval.c:10804`), whose `EXPR_INDEX` branch evaluates the index first (`eval.c:645`); running `idx()` triggers `gc_maybe_collect → gc_cycle`, which `fluid_sweep`s the unrooted RHS array. `*target = valr.value` then stores a dangling array.
- **Trigger:** `clat --tree-walk --gc-stress` on `flux arr=[9,9,9]; arr[idx()] = [1,2,3,4,5]; print(arr[0])`. ASan confirms heap-use-after-free; scope teardown double-frees.
- **Consequence:** UAF read + double-free; any `container[call_expr()] = heap_value` form is affected. Also reachable under natural heap pressure (`total_bytes >= gc_threshold`).
- **Suggested fix:** `GC_PUSH(ev, &valr.value)` immediately after evaluating the RHS and `GC_POP` after `*target = valr.value`.

---

#### H-6 — EXPR_CALL: argument array popped off GC shadow stack before higher-order builtins run user closures (UAF + double-free)
- **Location:** `src/eval.c:2081`
- **Category:** UAF (tree-walker)
- **Mechanism:** After evaluating args (each `GC_PUSH`'d), `GC_POP_N(ev, argc)` un-roots the entire `args` array before builtin dispatch. `pipe`/`grow`/`assert_throws`/`map`/`filter` then run a user closure whose body triggers GC, sweeping the still-needed sibling closure's `param_names`/name strings (fluid-allocated by `value_closure`, `value.c:247`). The next iteration reads freed metadata in `env_define` (`eval.c:1338`).
- **Trigger:** `clat --tree-walk --gc-stress` on `pipe(5, |x| { x * 2 }, |x| { x + 1 })`. ASan: heap-use-after-free READ in `call_closure`.
- **Consequence:** UAF of closure metadata + double-free on cleanup; affects `pipe` (`7287`), `grow` (`2510`), `assert_throws` (`7152`), `map`/`filter` (`5063`/`5081`).
- **Suggested fix:** Keep `args` rooted across the entire builtin dispatch — move `GC_POP_N` to after the builtin returns, or re-`GC_PUSH` the args inside the affected higher-order builtins.

---

#### H-7 — Off-by-one OOB read in `match` array-pattern with element(s) after a `...rest`
- **Location:** `src/eval.c:9740-9742`
- **Category:** OOB-read (tree-walker)
- **Mechanism:** The post-rest index formula `arr_idx = arr_len - (pat_count - 1 - k)` is one too high; correct is `arr_len - (pat_count - k)`. For the trailing element `k = pat_count-1` it computes `arr_idx = arr_len`, one past the end. The OOB `LatValue`'s type tag and union are then fed to `value_deep_clone` (`:9752`) or `value_equal` (`:9764`), which dereference union pointer members.
- **Trigger:** `clat --tree-walk` on `match arr { [first, ...mid, last] => { print(last) } _ => {} }` with `classify([10,20,30,40])`. ASan: heap-buffer-overflow READ in `value_clone_impl`.
- **Consequence:** OOB heap read with type-confusion potential (and silent mis-binding for non-final post-rest elements).
- **Suggested fix:** Change the formula to `arr_idx = arr_len - (pat_count - k)` for post-rest elements.

---

#### H-8 — `freeze(x) where <contract>` reads a non-closure value through the closure union → wild-pointer dispatch
- **Location:** `src/eval.c:9011-9020` (also `8671, 8735, 8786, 8970`)
- **Category:** type-confusion (tree-walker)
- **Mechanism:** The parser accepts any expression after `where` (`parser.c:1301`), but eval reads `cr.value.as.closure.{param_names,param_count,body,captured_env,…}` and calls `call_closure` with **no** `cr.value.type == VAL_CLOSURE` check. The closure struct aliases `array {elems,len,cap}`, so an array contract makes `param_names=elems`, `param_count=len`, `body=(Expr*)cap`, `captured_env` a wild pointer.
- **Trigger:** `clat --tree-walk` on `let x = freeze([1, 2, 3]) where [99]`. ASan: SEGV READ at `0x8` in `env_get → call_closure`.
- **Consequence:** Type confusion → wild-pointer dereference / reliable DoS, with a fabricated `Expr*` body later passed to `eval_expr`.
- **Suggested fix:** Validate `cr.value.type == VAL_CLOSURE` before invoking the contract at all five sites; otherwise raise a runtime type error.

---

#### H-9 — Bytecode verifier performs no stack-balance analysis; `pop()`/`OP_POP` underflow the value stack on crafted `.latc`
- **Location:** `src/stackvm.c:80-83` (and `OP_POP` `:3404-3408`); verifier `src/chunk.c:789-833`, invoked from `src/latc.c:603`
- **Category:** OOB read+write/arbitrary-free
- **Mechanism:** `chunk_verify` checks opcode lengths, operand ranges, jump targets, and a terminating op — but never models stack depth. `pop()` does a bare `vm->stack_top--; value_free(...)` with no underflow guard (only `push()` checks overflow). The top frame starts at `stack_top == vm->stack`, and `stack[]` is preceded in the struct by `frame_count`/`frames[512]`.
- **Trigger:** A crafted `.latc` such as `[OP_POP][OP_HALT]` (stack starts empty) or `[OP_NIL][OP_POP][OP_POP]…[OP_HALT]`. Passes verification; underflows immediately.
- **Consequence:** Surplus pops reinterpret live frame pointers as a `LatValue`; `value_free` frees a wild pointer (type confusion / arbitrary free), then continued underflow segfaults below the ~122KB allocation.
- **Suggested fix:** Add an abstract stack-depth pass to `chunk_verify` (track min/max height per instruction, require balance and non-negativity); optionally add a runtime lower-bound assert in `pop()`.

---

#### H-10 — Local-slot operand never bounds-checked by verifier or VM → OOB write + wild free on crafted `.latc`
- **Location:** `src/stackvm.c:3856-3868` (and `OP_INC/DEC/INDEX/GET_FIELD_LOCAL/FREEZE_VAR`); verifier `src/chunk.c` (`default: break` at `:779`)
- **Category:** OOB-write
- **Mechanism:** `OP_GET_LOCAL`/`OP_SET_LOCAL` index `frame->slots[slot]` with `slot = READ_BYTE()` (0–255) and no bound; `verify_instr_operands` has no case for the `*_LOCAL` opcodes. `frame->slots` points into the single shared `stack[4096]`; recursion (`FRAMES_MAX=512`) plus per-frame stack residency drives the frame base high, so `base + slot >= 4096` addresses `stack_top`/`env`/`handlers[]`/`defers[]`.
- **Trigger:** A crafted recursive function whose body executes `OP_SET_LOCAL 255` once the frame base exceeds ~3841 (reachable via recursion or ~3850 pushed constants).
- **Consequence:** `OP_GET_LOCAL` reads an OOB `LatValue`; `OP_SET_LOCAL` `value_free`s it (wild free of decoded struct fields) then overwrites — OOB read+write / arbitrary free from untrusted bytecode.
- **Suggested fix:** Add a `max_slots` field to `Chunk`, set at compile time; validate `slot < max_slots` in the verifier and bound the frame window at call time.

---

#### H-11 — Use-after-free of `LatRef` cell (and its mutex) in regvm `GETINDEX` Ref-proxy branch when dst aliases the Ref register
- **Location:** `src/regvm.c:3777-3799`
- **Category:** UAF (register VM, `--regvm`)
- **Mechanism:** For `r = r[0]` the compiler emits `GETINDEX` with `dst == obj` (`a==b`). `reg_set(&R[a], clone)` runs **before** `ref_unlock(ref)`: it saves `old = R[a]` (the sole `LatRef`, refcount 1), assigns the clone, then `value_free` → `ref_release` runs `pthread_mutex_destroy` + `free(ref)`. The subsequent `ref_unlock(ref)` then `pthread_mutex_unlock`s the freed, destroyed mutex.
- **Trigger:** `clat --regvm` on `fn go() { let r = Ref::new([10,20,30]); r = r[0]; return r } print(go())`. Same in the `VAL_MAP` branch via `r = r["key"]`. ASan confirms heap-use-after-free in `ref_unlock`.
- **Consequence:** UAF of the 144-byte `LatRef` + `pthread_mutex_unlock` on a destroyed mutex (UB); corruption under allocator reuse / concurrency.
- **Suggested fix:** Call `ref_unlock(ref)` **before** `reg_set`, or `ref_retain` across the `reg_set`, or special-case `a==b`.

---

#### H-12 — `regchunk_verify` omits register-window bounds checks for `ROP_CALL`/`ROP_PRINT`/`ROP_INVOKE*` → OOB register reads from crafted `.rlatc`
- **Location:** `src/regvm.c:4389-4395` (PRINT; CALL `:4035/4125`; INVOKE `:4419-4420`); verifier `:7342-7466`
- **Category:** OOB-read
- **Mechanism:** `RCK_WINDOW (base+count<=256)` is applied only to `ROP_NEWARRAY/NEWTUPLE/NEWSTRUCT/NEWENUM/FREEZE_EXCEPT`; `ROP_CALL`/`ROP_PRINT` hit `default: break`, and `ROP_INVOKE*` only validate the method-name constant. Each frame window is exactly 256 slots, but `R[a+i]` with uint8_t `a,b` reaches ~509 — past the window into adjacent frames, and at depth ~63 past the entire `reg_stack[16384]`.
- **Trigger:** A crafted `.rlatc` (any `RLAT`-magic file run by `clat`) with `ROP_PRINT A=255 B=102` after a callee writes a heap value into a high register.
- **Consequence:** OOB read of `LatValue` slots that `value_print`/`rvm_clone` then dereference → crash, heap info-disclosure, or read of leftover/freed slots. (Note: one refuter rated the UAF-via-dangling-slot variant lower since `value_free` `memset`s the slot; the cross-window OOB read remains.)
- **Suggested fix:** Add `RCK_WINDOW` checks for `ROP_CALL`, `ROP_PRINT`, and the `ROP_INVOKE` family in `reg_verify_operands`.

---

#### H-13 — Field/method/struct/enum/import name-constant opcodes lacking a 16-bit variant emit `(uint8_t)idx` with no guard (stack VM)
- **Location:** `src/stackcompiler.c:843, 931-933, 945-946, 1656-1657, 2382, 2757` (also `OP_GET_FIELD_LOCAL :831`); consumed at `src/stackvm.c:4568/4578` (BUILD_STRUCT), `:4883` (GET_FIELD), `:5082` (INVOKE)
- **Category:** type-confusion (default backend)
- **Mechanism:** These six opcodes bypass `emit_constant_idx` and have no `*_16` form (unlike `OP_INVOKE_LOCAL`/`OP_INVOKE_GLOBAL` which guard at `:869/:905`). With >256 constants the name-string index truncates and aliases an earlier non-string constant; the VM passes `constants[idx].as.str_val` straight to `intern()`/`strcmp`.
- **Trigger:** 300 distinct large-int constants then `let p = P { lateField: 5 }`. Default `clat` SIGSEGVs in `value_struct_vm → lat_strdup → strlen` reading the int reinterpreted as `char*`.
- **Consequence:** Wild-pointer read / crash (and OOB array read for `BUILD_STRUCT` field-name span) from ordinary large source — accidentally reachable in generated code (anneal/self-host).
- **Suggested fix:** Add 16-bit variants (or a compile-time `>255` guard) for `OP_GET_FIELD`, `OP_SET_FIELD`, `OP_INVOKE`, `OP_BUILD_STRUCT`, `OP_BUILD_ENUM`, `OP_IMPORT`, and `OP_GET_FIELD_LOCAL`.

---

#### H-14 — scope/spawn/select sub-chunk constant indices truncated to 8 bits → regvm executes wrong `RegChunk` / drops body / casts non-closure `native_fn`
- **Location:** `src/regcompiler.c:2499` (and `2534, 2551, 2595, 2598, 2603, 2607`); consumed at `src/regvm.c:5793/5816/5836/5942/5964/6039`
- **Category:** type-confusion (register VM)
- **Mechanism:** `add_regchunk_constant` returns a 16-bit index but `ROP_SCOPE`/`ROP_SELECT` operands are 8-bit. Sub-chunks are appended to the parent chunk's table, so >256 prior constants truncate the index. The regvm casts `constants[idx].as.closure.native_fn` to `RegChunk*` with no type check.
- **Trigger:** `clat --regvm` on (1) 300 float locals + `spawn { print(0) }` → body silently dropped; (2) 200 user fns + `scope { spawn { print(7) } }` → wrong RegChunk executed (`cannot add immediate to Nil`).
- **Consequence:** Silent dropped concurrency body, execution of an attacker-influenceable RegChunk, or wild `RegChunk*`/`char*` deref.
- **Suggested fix:** 16-bit operand forms for these sub-chunk indices, or `rc_error` on `>255`.

---

#### H-15 — Invalid `free()` of uninitialized pointer in interpolated-string error recovery
- **Location:** `src/parser.c:1266-1267`
- **Category:** uninitialized (shared frontend — all backends)
- **Mechanism:** `parts[]` is `malloc`'d (uninitialized). After a successful `parse_expr`, `count++` runs, but `parts[count]` is written only in the `MID`/`END` branches. The else branch `goto interp_fail` without writing `parts[count]`; cleanup `for (i=0; i<=count; i++) free(parts[i])` then frees the uninitialized slot.
- **Trigger:** `print("${1 2}")` (interpolation body that is not a single complete expression). ASan: SEGV in `free` with `0xbebebebebebebebe`.
- **Consequence:** Wild free of an uninitialized pointer — heap corruption / DoS, crashes the parser on every backend from trivial source.
- **Suggested fix:** Use `i < count` in the cleanup loop, or initialize `parts[count] = NULL` before parsing each interp expression.

---

#### H-16 — TOML array-of-tables grows a `value_array` buffer with libc `realloc()`, corrupting the heap tracker → double-free
- **Location:** `src/toml_ops.c:455-463`
- **Category:** double-free
- **Mechanism:** The array buffer originates from `value_array` → `lat_alloc` → `fluid_alloc`, which records the pointer in the heap's tracking list. Calling bare libc `realloc()` frees/moves that block and returns an untracked pointer, leaving a stale `FluidAlloc` node. At teardown `fluid_heap_free` `free`s the stale pointer = double-free. (`lat_realloc_routed`/`fluid_realloc` exist for exactly this; TOML bypasses them.)
- **Trigger:** Parse TOML with 5+ `[[a]]` headers (initial cap 4) under `--tree-walk` or inside a spawned VM fiber (any context where `g_heap` is set). ASan confirms double-free.
- **Consequence:** Heap double-free / allocator-tracking corruption from attacker-controlled TOML (config/manifest/network).
- **Suggested fix:** Use `lat_realloc_routed`/`fluid_realloc` (as `eval.c:7710` does), or the existing array-grow helper, instead of raw `realloc`.

---

#### H-17 — `random_int(INT64_MIN, INT64_MAX)`: signed overflow makes `range==0` → division by zero (SIGFPE on x86-64/Windows)
- **Location:** `src/math_ops.c:219-220`
- **Category:** int-overflow
- **Mechanism:** The only guard is `lo > hi`. With `lo=INT64_MIN, hi=INT64_MAX`, `hi - lo` overflows int64 to `0xFFFF…FF`; `(uint64_t)(…)+1` wraps to `range==0`; `rand() % 0` divides by zero.
- **Trigger:** `random_int(0 - 9223372036854775807 - 1, 9223372036854775807)`. UBSan: signed overflow at `:219` and division by zero at `:220`.
- **Consequence:** SIGFPE crash (DoS) on x86-64/Windows release targets from a one-line script; garbage out-of-distribution value on arm64; signed-overflow UB the optimizer may exploit.
- **Suggested fix:** Special-case the full-span (`range==0`) to emit a full-width `rand()`-derived value, and compute the span in unsigned arithmetic without `hi - lo` signed overflow.

---

#### H-18 — `native_load_bytecode` casts `active_vm` to `StackVM*` without checking backend → type confusion / SIGSEGV under `--regvm`
- **Location:** `src/runtime.c:3779-3781`
- **Category:** type-confusion (register VM)
- **Mechanism:** The function unconditionally does `StackVM *vm = (StackVM *)current_rt->active_vm; stackvm_run(vm, …)` with no `rt->backend` guard, despite siblings (`native_require :2958`, `native_lat_eval :3403`, `native_async_iter :2535`) all guarding. Under `--regvm`, `active_vm` is a `RegVM*`; `stackvm_run` immediately writes `vm->rt->…` through wrong offsets.
- **Trigger:** `clat --regvm loader.lat` where `loader.lat` calls `load_bytecode("inner.latc")`. Exit 139; lldb: `EXC_BAD_ACCESS` in `stackvm_run`.
- **Consequence:** Type confusion → wild reads/writes over the `RegVM` struct, not a clean abort.
- **Suggested fix:** Add `if (current_rt->backend == RT_BACKEND_REG_VM) { … }` mirroring `native_require`/`native_lat_eval` (either a proper regvm path or a graceful error).

---

#### H-19 — `native_breakpoint` casts `active_vm` to `StackVM*` without checking backend → SIGSEGV under `--regvm`
- **Location:** `src/runtime.c:3521`
- **Category:** type-confusion (register VM)
- **Mechanism:** `StackVM *vm = (StackVM *)rt->active_vm;` with no backend guard; reads `vm->frame_count`/`vm->frames[...]` at wrong offsets and calls `stackvm_run(vm, …)` (`:3603`) on the `RegVM*` for any entered expression.
- **Trigger:** `printf 'x + 1\ncont\n' | clat --regvm prog.lat` where `prog.lat` calls `breakpoint()`. Exit 139.
- **Consequence:** Type-confused reads + `stackvm_run` writing over `RegVM` memory. Note: trigger surface is developer-only (explicit `breakpoint()`, `--regvm`, interactive input); two refuters rated practical impact medium.
- **Suggested fix:** One-line backend guard: under `RT_BACKEND_REG_VM` either dispatch to a RegVM mini-REPL or no-op with a message.

---

#### H-20 — Path traversal / arbitrary-directory file write via unsanitized registry-returned version string
- **Location:** `src/package.c:498-565`
- **Category:** other (supply-chain)
- **Mechanism:** `registry_fetch_versions` extracts version strings verbatim; for constraint `"*"`, `pkg_semver_satisfies` returns true unconditionally and `find_best_version` returns `versions[0]` unchanged. The result `resolved` is used as a path component in `find_cached_package` (`:510`) and `pkg_cache` (`:562`) and is **never** passed through `pkg_path_component_is_safe` — unlike the manifest version, which **is** validated at `:677`. `fs_mkdir` and `builtin_write_file` resolve `..` through the pre-created `name` subdir.
- **Trigger:** `evil = "*"` in `lattice.toml`, `clat install` against an attacker-controlled/MITM'd registry returning `{"versions":["../../../../../../../../tmp/pwned"]}`.
- **Consequence:** Registry-controlled arbitrary-directory write of attacker content (e.g. `main.lat`/`lattice.toml`) outside the cache — supply-chain code-execution / file-clobber primitive.
- **Suggested fix:** Validate `resolved` with `pkg_path_component_is_safe()` immediately after `find_best_version` (mirroring the manifest check at `:677`); reject `..`/`/` segments.

---

#### H-21 — `Env` reference count is non-atomic while envs are shared across threads via closure capture → data race / premature free
- **Location:** `src/env.c:39-46`
- **Category:** UAF (tree-walker concurrency)
- **Mechanism:** `env_retain`/`env_release` use plain `++`/`--` on a non-atomic `size_t` (`env.h:15`), unlike `channel.c:19-22` which uses `__atomic` for the same cross-thread pattern. `value_deep_clone` of a closure does `env_retain(captured_env)` rather than deep-copying (`value.c:522`), so sibling spawn threads share the same `Env E` and concurrently `env_detach_values` → `env_retain`/`env_release` on `E->refcount`.
- **Trigger:** `fn make() { flux n=0; return || { n } } let f = make(); scope { spawn { 1 } spawn { 2 } }` under `--tree-walk`.
- **Consequence:** Lost update on `refcount` → premature `env_free` while live closures still reference `E` → UAF / double-free of scope hashmaps (timing-dependent).
- **Suggested fix:** Make `Env.refcount` `_Atomic` and use `__atomic_add_fetch`/`__atomic_sub_fetch` (SEQ_CST), matching the channel refcount.

---

### MEDIUM

---

#### M-22 — `OP_BUILD_TUPLE` leaks every popped element (value_tuple deep-clones; originals never freed)
- **Location:** `src/stackvm.c:4547-4559`
- **Category:** leak (default backend)
- **Mechanism:** The opcode pops `count` **owned** values into `elems[]`, then `value_tuple` (`value.c:308`) deep-clones each; the originals are never `value_free`'d (only the `elems` array is freed when `count>16`). Contrast `OP_BUILD_ARRAY`/`OP_BUILD_STRUCT`, whose constructors move (memcpy/assign).
- **Trigger:** `let s = "...long..."; let t = (s, 1)` in a loop. 2M iterations peak at 454 MB vs 24 MB baseline.
- **Consequence:** Unbounded heap growth → OOM/DoS for any tuple with non-primitive elements.
- **Suggested fix:** `value_free(&elems[i])` for each element after `value_tuple`, or give `value_tuple` move semantics.

---

#### M-23 — `OP_BUILD_ENUM` leaks every popped payload value (value_enum deep-clones; originals never freed)
- **Location:** `src/stackvm.c:4632-4648`
- **Category:** leak (default backend)
- **Mechanism:** Same clone-vs-move mismatch: `value_enum` (`value.c:289`) deep-clones each payload; `free(payload)` releases only the array, not the `LatValue`s. Affects idiomatic `Some(x)`/`Ok(data)`/`Box::Wrap(s)`.
- **Trigger:** `Box::Wrap(s)` / `Box::Wrap([i,i+1,i+2])` in a loop. 2M iterations ~548 MB; array payload ~901 MB.
- **Consequence:** Unbounded growth → OOM/DoS.
- **Suggested fix:** `value_free` each popped payload element after `value_enum`, or give `value_enum` move semantics.

---

#### M-24 — Conditional/iter/handler jump offsets silently truncated to `int16_t` with no range check
- **Location:** `src/regcompiler.c:195`
- **Category:** logic (register VM)
- **Mechanism:** `patch_jump` casts the forward distance to `int16_t` (AsBx 16-bit field) with no overflow detection. Unconditional jumps use a 24-bit `sBx`, but `JMPFALSE/JMPTRUE/JMPNOTNIL/ITERNEXT/PUSH_HANDLER` cannot be widened. The regvm does `frame->ip += offset` with no ip-bounds check.
- **Trigger:** A function/if-else/match/try body where a forward conditional jump spans >32767 instructions (large/generated code).
- **Consequence:** Wrapped (possibly negative) offset → wrong/backwards control flow, infinite loops, OOB instruction fetch — silent, no diagnostic.
- **Suggested fix:** Range-check the distance and `rc_error` on overflow (mirroring the `LOADI` int16 fallback at `:539/:556`), or add a 24-bit conditional-jump form.

---

#### M-25 — Unbounded recursion in `value_free`/`value_clone_impl` (and the recursive value-op family) → stack-overflow on deeply nested data
- **Location:** `src/value.c:1515-1517` (and `value_clone_impl :444`, `value_eq`, `value_display`, `set_phase_recursive`, `region_tag_recursive`, etc.)
- **Category:** other (DoS, default backend via `value_clone_fast`)
- **Mechanism:** Arrays/structs/tuples/enums recurse one C frame per nesting level with no depth cap. `flux a=[0]; for i in 0..200000 { a=[a] }` exhausts the ~8MB stack at ~50k–80k levels in clone, free, equality, print, and phase walks.
- **Trigger:** Deeply nested runtime value. Reproduced: SIGSEGV (exit 139) on default backend via `value_clone_fast`; 1000-deep crashes under `ulimit -s 256`.
- **Consequence:** Whole-process stack-overflow SIGSEGV (DoS) — relevant for WASM playground / anneal server contexts.
- **Suggested fix:** Convert the core value walks to iterative (explicit stack), or add a configurable recursion-depth guard that returns an error instead of recursing past a limit.

---

#### M-26 — `gc_mark_value` recurses without a visited guard → stack-overflow on reference cycles
- **Location:** `src/gc.c:123-218` (VAL_REF at `208-210`)
- **Category:** other (DoS, opt-in GC only)
- **Mechanism:** `gc_mark_value` never checks the already-marked bit before recursing into `VAL_ARRAY`/`VAL_STRUCT`/`VAL_REF`; the only early-out is `region_id != REGION_NONE`, which a fully-`REGION_NONE` cycle doesn't trip. A `Ref` cycle (`r.set([r])`) recurses forever. The incremental path (`gc_gray_push_value`) has the same defect (unbounded gray stack).
- **Trigger:** `clat --gc-stress` on `flux r = Ref::new(0); r.set([r])`. Exit 139; lldb shows unbounded `gc_mark_value` frames.
- **Consequence:** Stack-overflow SIGSEGV (or OOM/null-deref in incremental mode). Note: reachable only under opt-in `--gc`/`--gc-stress`/`--gc-incremental`; default backend uses arena+refcounting (two refuters rated low).
- **Suggested fix:** Check `obj->marked` before recursing (return if already marked); the same already-marked short-circuit fixes the incremental gray-push path.

---

#### M-27 — 32-bit unsigned overflow in `malloc(slen+1)` for length-prefixed strings → heap buffer overflow
- **Location:** `src/latc.c:348` (also `426, 450, 852, 938`)
- **Category:** int-overflow (untrusted `.latc`)
- **Mechanism:** `slen` is `uint32_t`; `slen + 1` wraps in 32-bit arithmetic to 0 at `slen==0xFFFFFFFF` → `malloc(0)`. `br_read_bytes` then `memcpy`s up to `0xFFFFFFFF` bytes. Line `520` uses the correct `malloc((size_t)slen + 1)`, proving the omission is a bug.
- **Trigger:** A `.latc` with `slen == 0xFFFFFFFF`. On 64-bit needs a ~4GB file; on **wasm32/32-bit** the `br_read_bytes` bounds check also wraps, so a small file triggers it.
- **Consequence:** Heap buffer overflow with attacker-controlled content (plus OOB `s[slen]='\0'`); trivially exploitable on wasm32/32-bit.
- **Suggested fix:** Cast `(size_t)slen + 1` at all five sites; additionally bound `slen` against remaining input bytes and add the missing `!s` check at `:852`.

---

#### M-28 — Attacker-controlled `line_count` used as allocation size with unchecked `realloc` (and 32-bit multiply overflow)
- **Location:** `src/latc.c:281-293` (also `785-797`)
- **Category:** null-deref / int-overflow (untrusted `.latc`)
- **Mechanism:** `line_count` is read raw and used immediately as the `realloc` element count before any line data is read; the result is never NULL-checked before `c->lines[i] = …`. On 32-bit/wasm32, `lines_cap * sizeof(int)` truncates → undersized buffer the loop overflows.
- **Trigger:** A ~21-byte `.latc` with `line_count = 0xFFFFFFFF` and ≥4 bytes of line data (16GB realloc).
- **Consequence:** NULL-deref crash on constrained/wasm builds; heap overflow on 32-bit. (Benign on 64-bit overcommit hosts — why the 64KB fuzzer misses it.)
- **Suggested fix:** Bound `line_count` by remaining input bytes, use `(size_t)line_count * sizeof(int)`, and check the `realloc` result for NULL.

---

#### M-29 — Unbounded recursion in `parse_pattern` → C stack overflow (DoS)
- **Location:** `src/parser.c:1134` (also enum-payload `:1061`, struct-field `:1179`)
- **Category:** other (DoS, shared frontend)
- **Mechanism:** `parse_pattern` never touches `p->depth`; the `PARSER_MAX_DEPTH` guard exists only in `parse_expr` (`:539`). Nested array patterns recurse one frame per `[`.
- **Trigger:** `match 0 {` followed by ~60000 `[`. ASan: stack-overflow in `parse_pattern`.
- **Consequence:** Process crash on untrusted source (all backends).
- **Suggested fix:** Add a `p->depth`/`PARSER_MAX_DEPTH` increment/decrement at `parse_pattern` entry, as `parse_expr` does.

---

#### M-30 — Unbounded recursion in `parse_type_expr` → C stack overflow (DoS)
- **Location:** `src/parser.c:246` (also fn params `:274`, return `:311`, generics `:343`)
- **Category:** other (DoS, shared frontend)
- **Mechanism:** No depth guard on nested array/fn/generic type expressions. Reachable from any type-annotation position (let bindings call `parse_type_expr` directly at `:2286`, bypassing the `parse_expr` guard).
- **Trigger:** `let x: ` + ~60000 `[` + `Int` (ASan threshold; release binary crashes at ~200000). Exit 139.
- **Consequence:** Stack-overflow crash on untrusted source.
- **Suggested fix:** Add a depth counter to `parse_type_expr` mirroring `parse_expr`'s `p->depth`/`PARSER_MAX_DEPTH`.

---

#### M-31 — Unbounded recursion in YAML nested-mapping value parsing bypasses `YAML_MAX_DEPTH`
- **Location:** `src/yaml_ops.c:431` (and sequence map-key continuation `:354`)
- **Category:** other (DoS)
- **Mechanism:** The block-mapping value recursion (and the sequence map-key continuation) omit the `if (p->depth >= YAML_MAX_DEPTH){…} p->depth++` guard that every other recursion site uses, so `p->depth` stays 0 on a pure nested-mapping path.
- **Trigger:** `yaml_parse` on a deeply nested block mapping (`a:\n a:\n  a:…`). Reproduced ~5000–6000 levels → stack-overflow. Worker threads (~512KB stack) need far less.
- **Consequence:** Stack-overflow crash from attacker-supplied YAML.
- **Suggested fix:** Add the same `YAML_MAX_DEPTH` guard + `p->depth++`/`--` around the recursive calls at `:431` and `:354`.

---

#### M-32 — `Set.clear()` leaks all stored element values (`lat_map_free` does not free inline values)
- **Location:** `src/builtin_methods.c:1591-1598`
- **Category:** leak
- **Mechanism:** Set elements are heap-owning `value_deep_clone` copies; `lat_map_free` frees only keys + the entries array ("value is inline — no free needed"). `value_free`'s `VAL_SET` case frees each value first precisely because `lat_map_free` doesn't — `clear()` bypasses that.
- **Trigger:** `loop { s.add(big); s.clear(); }` with non-scalar elements.
- **Consequence:** Permanent leak of each element's heap (strings/arrays/structs/maps) → DoS.
- **Suggested fix:** Iterate occupied entries and `value_free` each value before `lat_map_free` (reuse the `VAL_SET` teardown logic).

---

#### M-33 — `Map.remove()` leaks the removed value (`lat_map_remove` does not free inline value)
- **Location:** `src/builtin_methods.c:1103-1108`
- **Category:** leak
- **Mechanism:** The map owns heap value clones, but `lat_map_remove` only frees the key + tombstones the slot; the dispatcher frees only the key argument. The tombstoned value is skipped even at map teardown. The tree-walker does it correctly (`eval.c:7933-7936`: `value_free(old)` before remove) — the two VM backends omit that step (`regvm.c:2134` too).
- **Trigger:** `loop { m["k"]=big; m.remove("k"); }`. RSS scales linearly with iterations.
- **Consequence:** Unbounded leak → OOM/DoS in long-running servers (anneal).
- **Suggested fix:** In `builtin_map_remove`, fetch the value via `lat_map_get` and `value_free` it before `lat_map_remove`, matching the tree-walker.

---

#### M-34 — `Buffer.write_u64/i64/u8/u16/u32` read `args[1].as.int_val` with no type check → host-pointer disclosure
- **Location:** `src/builtin_methods.c:1319-1320` (also `1187, 1209, 1234, 1351`)
- **Category:** type-confusion (info-disclosure)
- **Mechanism:** Write methods validate only the index (`args[0]`) is `VAL_INT`; the value (`args[1]`) is read via `.as.int_val` regardless of type. The union overlaps `int_val` with `str_val`/`array.elems`/closure pointers, so a non-Int argument writes the raw 8-byte host pointer. The `push`/`fill` methods guard correctly, confirming the omission is unintended.
- **Trigger:** `b.write_u64(0, "x"); let p = b.read_u64(0)` → `p` is the heap address of `"x"`. Reproduced on the default backend (0x60… ASan heap address).
- **Consequence:** Host heap/code pointer disclosure to untrusted script → ASLR defeat (read primitive; the index remains bounds-checked so no OOB write).
- **Suggested fix:** Validate `args[1].type == VAL_INT` in every `write_*` method before reading `int_val`.

---

#### M-35 — `parse_response` reads uninitialized/OOB heap when server closes with no data
- **Location:** `src/http.c:137-143`
- **Category:** uninitialized
- **Mechanism:** `resp_buf = malloc(8192)` is never `memset` or NUL-terminated. On a clean EOF with no bytes, `net_tcp_read`/`net_tls_read` return a non-NULL empty string, so the `!chunk` error branch is skipped and the loop breaks with `resp_len==0`; `parse_response` is called unconditionally and `strstr` scans uninitialized heap with no guaranteed terminator.
- **Trigger:** An HTTP(S) request to a server that accepts and immediately closes the connection.
- **Consequence:** Read of uninitialized heap; possible OOB read past the allocation → crash/DoS, or mis-parse of stale heap. (One refuter rated low: fresh mmap pages zero out and the `HTTP/` guard limits mis-parse.)
- **Suggested fix:** Short-circuit `if (resp_len == 0) return error(...)` before `parse_response`, and/or `resp_buf[0] = '\0'` immediately after `malloc`.

---

#### M-36 — `channel_add_waiter` inserts one waiter node into multiple channel lists via a single `next` pointer → lost wakeups / select hang
- **Location:** `src/channel.c:139-149`
- **Category:** logic (concurrency)
- **Mechanism:** `LatSelectWaiter` has a single `next`, but each `select` registers its one stack waiter on **every** channel arm (`eval.c:10234`, `stackvm.c:8238`, `regvm.c:6100`). When two concurrent selects share a channel, registering a waiter on a second channel overwrites the `next` link that belonged to the shared list, silently dropping a sibling waiter. Channels are shared by reference (`value.c:538` retains, no deep clone). There is also a data race on `w->next` written under two different per-channel mutexes.
- **Trigger:** Two spawned tasks each `select { recv(shared) => {} recv(private) => {} }` while a third sends on `shared`.
- **Consequence:** A sender signals only the surviving node; the dropped select blocks forever in `pthread_cond_wait` → deadlock/hang (enclosing `pthread_join` never returns). Race-dependent.
- **Suggested fix:** Give each select arm its **own** waiter node (allocate one node per channel arm), so a node belongs to exactly one channel list.

---

#### M-37 — `process_shell()` deadlocks when child writes large output to both stdout and stderr
- **Location:** `src/process_ops.c:245-246`
- **Category:** other (DoS)
- **Mechanism:** The parent drains the two pipes strictly sequentially with blocking reads (`read_all_fd` reads stdout to EOF before touching stderr), no `poll`/`select`/non-blocking fds. If the child fills the ~64KB stderr pipe buffer it blocks on `write(stderr)` and never closes stdout; the parent blocks reading stdout forever.
- **Trigger:** `shell("perl -e 'print STDOUT q{a}; print STDERR q{b} x 200000; print STDOUT q{c} x 200000'")` — any command emitting >64KB stderr with stdout pending.
- **Consequence:** Classic two-pipe deadlock → interpreter hangs indefinitely, even on well-behaved verbose commands.
- **Suggested fix:** Drain both pipes concurrently — `poll()`/`select()` over both fds, or set them non-blocking, or use a second thread.

---

### LOW

---

#### L-38 — `add_local` has no 256-slot cap; local slots emitted as 1-byte operands silently truncate
- **Location:** `src/stackcompiler.c:194-206, 483, 2367`
- **Category:** int-overflow (correctness, not memory-unsafe)
- **Mechanism:** `add_local` doubles `local_cap` indefinitely and never errors at 256, but every `OP_*_LOCAL` emit casts the slot to `(uint8_t)`. The upvalue path guards at 256 (`:222`); locals do not. Slot >255 wraps modulo 256, aliasing in-bounds slots (including reserved self-slot 0).
- **Trigger:** A single function/scope with >256 locals (e.g. 257 sequential `let` bindings; or large destructuring). Reproduced: returns the wrong local's value, no diagnostic, ASan-clean.
- **Consequence:** Silent miscompilation — variables read/clobber each other. In-bounds, so no memory unsafety.
- **Suggested fix:** Emit `compile_error("too many local variables in function")` when `local_count >= 256` (mirroring the upvalue guard), or add 16-bit local opcodes.

---

#### L-39 — Unchecked arena page allocation result dereferenced in `arena_alloc` / `bump_alloc`
- **Location:** `src/memory.c:533-534` (also `239-241`)
- **Category:** null-deref (OOM-gated)
- **Mechanism:** `arena_page_new`/`arena_page_new_aligned` return NULL on OOM; both growth sites immediately do `np->next = …` with no NULL check (contrast `arena_calloc`/`crystal_region_create_shared` which check).
- **Trigger:** A crystal-region or ephemeral-bump allocation needing a fresh page when `malloc`/`posix_memalign` fails (real memory pressure, `RLIMIT_AS`, oversized page request, or wasm32 cap).
- **Consequence:** NULL-deref crash on allocation failure. (Pervasive infallible-allocation assumption across the arena subsystem.)
- **Suggested fix:** NULL-check the page allocation and propagate OOM (return NULL / longjmp to an error handler) at both sites.

---

#### L-40 — Integer literal overflow silently saturates to `LLONG_MAX` (`strtoll` errno unchecked)
- **Location:** `src/lexer.c:208`
- **Category:** int-overflow (correctness)
- **Mechanism:** `strtoll`'s `ERANGE`/errno is ignored; an out-of-range decimal/hex literal clamps to `LLONG_MAX`/`LLONG_MIN` with no diagnostic.
- **Trigger:** `print(99999999999999999999999)` or `print(0xFFFFFFFFFFFFFFFFFFFF)` → both yield `9223372036854775807`.
- **Consequence:** Wrong integer constant (can corrupt size/bounds computations) with no error. (The `strtod` path is unreachable for huge exponents — no `e/E` syntax.)
- **Suggested fix:** Clear `errno` before `strtoll`, and after the call emit a lexer error if `errno == ERANGE`.

---

#### L-41 — Unchecked `realloc()` in JSON string/array growth → NULL-deref write on allocation failure
- **Location:** `src/json.c:110-141, 253`
- **Category:** null-deref / leak (OOM-gated)
- **Mechanism:** `buf = realloc(buf, cap)` / `elems = realloc(elems, …)` followed by `buf[len++]=…` / `elems[len++]=…` with no NULL check; sibling `malloc`s at `60, 195, 223` and `jp_error` (`33`) **are** checked, so this is an asymmetric gap. On failure the old block leaks and the write goes through NULL+offset.
- **Trigger:** `json_parse()` on a very large attacker JSON under memory pressure, or deterministically on wasm32/32-bit (capped linear memory).
- **Consequence:** NULL-deref crash / WASM trap (DoS).
- **Suggested fix:** Check each `realloc` result for NULL and bail via `jp_error`.

---

#### L-42 — `\u` escape handling: surrogate pairs not decoded (lone-surrogate/CESU-8) and `\u0000` truncates string
- **Location:** `src/json.c:93-129`
- **Category:** logic (data integrity)
- **Mechanism:** The `\u` handler encodes each 4-hex code unit independently with no high/low surrogate combining and no surrogate-range rejection, so non-BMP code points become two 3-byte lone-surrogate sequences. `\u0000` writes an embedded NUL that `value_string` → `lat_strdup` (strlen-based) then truncates.
- **Trigger:** `json_parse("\"\\uD834\\uDD1E\"")` → `ED A0 B4 ED B4 9E` instead of `F0 9D 84 9E`; `json_parse("\"a\\u0000b\"")` → `"a"`.
- **Consequence:** Invalid (WTF-8) UTF-8 round-trip for non-BMP chars; silent truncation at embedded NUL. No memory-safety impact.
- **Suggested fix:** Detect a high surrogate (`0xD800–0xDBFF`) and combine with the following `\uDCxx` into a 4-byte UTF-8 sequence; use a length-carrying string constructor (`value_string_owned_len`) to preserve embedded NULs.

---

#### L-43 — `Set.remove()` leaks the removed element value (`lat_map_remove` does not free inline value)
- **Location:** `src/builtin_methods.c:1455-1462`
- **Category:** leak
- **Mechanism:** Same root cause as M-33: the set stores `value_deep_clone` copies, but `lat_map_remove` never frees the inline value and the dispatcher frees only the key argument. The tree-walker frees correctly (`eval.c:7975-7977`); the VM/regvm paths omit it.
- **Trigger:** `loop { s.add(big); s.remove(big); }` with a non-interned element (string >64 chars, array, struct). (Note: short string literals are interned and don't leak.)
- **Consequence:** Slow unbounded leak.
- **Suggested fix:** `value_free` the stored element (via `lat_map_get`) before `lat_map_remove`, matching the tree-walker.

---

#### L-44 — `pow(INT64_MIN, even exponent)`: `llabs(INT64_MIN)` UB bypasses overflow guard, then `base_val*=base_val` overflows
- **Location:** `src/math_ops.c:126-129`
- **Category:** int-overflow (UB / correctness)
- **Mechanism:** For an even exponent the line-117 guard is skipped; control reaches the squaring guard where `llabs(INT64_MIN)` is UB (returns `INT64_MIN`), making `INT64_MAX / llabs(...) == 0` so the `>0` test is false and the float fallback is not taken; `base_val *= base_val` then overflows.
- **Trigger:** `pow(0 - 9223372036854775807 - 1, 2)`. UBSan: negation-of-INT64_MIN and signed overflow; returns 0 (vs the float result on optimized builds — observable UB).
- **Consequence:** UB; silently wrong result on mainstream platforms, latent miscompilation risk.
- **Suggested fix:** Special-case `base == INT64_MIN`, and compute the magnitude/overflow check using unsigned arithmetic instead of `llabs`.

---

#### L-45 — Out-of-range / NaN double-to-int64 conversions are undefined behavior (`to_int`, `floor`, `ceil`, `round`)
- **Location:** `src/type_ops.c:15` (also `src/math_ops.c:40, 53, 66`)
- **Category:** int-overflow (UB / correctness)
- **Mechanism:** `(int64_t)v->as.float_val` with no `isnan`/`isinf`/range guard; converting a double outside int64 range (or NaN/inf) is UB.
- **Trigger:** `to_int(pow(10.0, 30.0))` (UBSan: "1e+30 is outside the range of representable values"), `floor(big)`, `to_int(0.0/0.0)`.
- **Consequence:** Platform-dependent garbage (arm64 saturates to INT64_MAX, x86-64 yields INT64_MIN); no crash on mainstream hardware but silently wrong, and UB the compiler may assume away.
- **Suggested fix:** Before the cast, check `isnan`/`isinf` and the `[INT64_MIN, INT64_MAX]` range; clamp or raise a runtime error.

---

## 3. Coverage & Gaps

### Subsystems scanned (with confirmed findings)
- **Tree-walker:** `src/eval.c` (+ `value.c`, `value.h`, `ast.h`) — GC root-set UAFs (H-5, H-6), match-pattern OOB (H-7), freeze-contract type confusion (H-8), Env refcount race (H-21).
- **Stack VM (default backend):** `src/stackvm.c`, `src/stackcompiler.c` (+ `stackopcode.h`, `chunk.c`, `memory.c`) — sub-chunk and name-constant truncation (C-1, H-13), verifier stack/local gaps (H-9, H-10), build-tuple/enum leaks (M-22, M-23), local-slot cap (L-38).
- **Register VM (`--regvm`):** `src/regvm.c`, `src/regcompiler.c` (+ `regopcode.h`) — field/name truncation (C-2), Ref UAF (H-11), verifier window gap (H-12), sub-chunk truncation (H-14), jump truncation (M-24).
- **Value core / memory / GC:** `src/value.c`, `src/value.h`, `src/memory.c`, `src/gc.c`, `memory.h`, `gc.h` — recursion DoS (M-25, M-26), arena OOM null-deref (L-39).
- **Bytecode serialization:** `src/chunk.c`, `src/latc.c`, `chunk.h`, `latc.h` — string/line-table deserialization overflow (M-27, M-28).
- **Frontend:** `src/parser.c`, `src/lexer.c` (+ `ast.h`, `token.h`, `lexer.h`) — interp uninitialized free (H-15), pattern/type recursion DoS (M-29, M-30), literal saturation (L-40).
- **Data formats:** `src/json.c`, `src/yaml_ops.c`, `src/toml_ops.c` — TOML realloc double-free (H-16), YAML recursion (M-31), JSON realloc/UTF-8 (L-41, L-42).
- **Stdlib builtins:** `src/builtin_methods.c`, `src/string_ops.c`, `src/math_ops.c`, `src/array_ops.c`, `src/iterator.c`, `src/type_ops.c` — random_int SIGFPE (H-17), Set/Map leaks (M-32, M-33, L-43), Buffer pointer leak (M-34), pow/cast UB (L-44, L-45).
- **Runtime / networking / packaging / concurrency / process:** `src/runtime.c`, `src/tls.c`, `src/http.c`, `src/net.c`, `src/package.c`, `src/channel.c`, `src/env.c`, `src/process_ops.c` — net/tls heap overflows (C-3, C-4), backend cast confusion (H-18, H-19), path traversal (H-20), HTTP uninitialized read (M-35), select waiter corruption (M-36), shell deadlock (M-37).
- Also in-scope per the scan list: `src/crypto_ops.c`, `src/fs_ops.c`, `src/path_ops.c`, `src/env_ops.c`, `src/format_ops.c` — no confirmed findings surfaced.

### Areas NOT covered — recommended follow-up pass
The following subsystems were **outside** this scan's lens and should receive a dedicated pass, especially since several process untrusted or externally-derived input:
- **`lsp_server.c` (LSP)** — parses untrusted editor JSON-RPC and source buffers continuously; high-value for the parser-recursion and deserialization defect classes found here.
- **`formatter.c`** — operates on untrusted source; AST/token walks are prime candidates for the same unbounded-recursion DoS as `parse_pattern`/`parse_type_expr`.
- **`doc_gen.c`** — `python3 scripts/generate_docs.py` plus the C doc extractor; string handling over arbitrary source comments.
- **Debugger / DAP adapter** — given the `breakpoint()` backend-cast confusion (H-19), the full debug-adapter surface and `breakpoint`/REPL eval paths warrant their own review.
- **`wasm_api.c` / WASM build** — multiple findings (C-4 Schannel-style overflows, M-27/M-28 32-bit truncation, L-41) are *more* exploitable on the 32-bit wasm32 target; the WASM entry/boundary code was not directly reviewed.
- **Self-hosted compiler (`compiler/latc.lat`) and `framework/anneal/`** — these generate large chunks and large maps, making the >256-constant truncation classes (C-1, C-2, H-13, H-14) and the leak classes (M-22, M-23, M-32, M-33) likely to fire in practice; worth a targeted differential test.
- **`crypto_ops.c`** — listed as scanned but produced no findings; given its security sensitivity, a deeper dedicated review (constant-time comparisons, key/nonce handling, RNG sourcing) is advisable rather than concluding it is clean.
- **CLI / `main.c` argument and file-loading paths** beyond the specific `run_latc_file`/`regchunk_load` flows referenced above.

**Priority recommendation:** Address the two systemic root causes first — (a) introduce 16-bit operand forms (or hard compile-time guards) for every truncating index emit in both compilers (C-1, C-2, H-13, H-14, L-38, M-24), and (b) complete the bytecode verifiers with stack-balance, local-slot, and register-window analysis (H-9, H-10, H-12) plus the `.latc`/`.rlatc` size/length validation (M-27, M-28). These cover the majority of the critical/high memory-safety surface. The net/tls length checks (C-3, C-4) and the path-traversal validation (H-20) are small, self-contained fixes with outsized impact.

---

*Machine-readable findings: see `security-audit-2026-06-27-findings.json`.*
