# Follow-Up Adversarial Audit — Lattice Auxiliary Subsystems

## 1. Executive Summary

This follow-up pass targeted the subsystems explicitly excluded from the first audit (LSP, formatter, doc-gen, crypto, WASM boundary, debugger/DAP, CLI/file-load, FFI) and produced **20 majority-confirmed findings**: **2 Critical, 5 High, 6 Medium, 7 Low**. The security posture of the *tooling/edge* surface is materially weaker than the core runtime the first scan reviewed: whereas the core interpreter/VM proved robust, memory-corruption defects here cluster in two places — the editor-facing **LSP** (two critical heap overflows building signatures, plus several crash-DoS primitives) and the **wasm32 sandbox boundary** (a matched OOB read/write pair and a giant OOB memset, all from 32-bit `size_t` truncation that is latent and harmless on 64-bit hosts). The **DAP debugger** contributes two use-after-frees rooted in a non-owning varref store, and the **FFI loader** carries a classic CWD library-planting RCE. Net: the language core is sound, but the surrounding developer tooling and the wasm playground are the real attack surface and need hardening before they can be trusted with untrusted `.lat` input.

---

## 2. Findings by Severity

### CRITICAL

#### C1. Heap overflow building function signature — type-annotation lengths uncounted
- **Location:** `src/lsp_analysis.c:112-124` (malloc:114, overflowing snprintf:122/124)
- **Category:** OOB-write
- **Mechanism:** `siglen = strlen(fn->name)+32` plus `strlen(params[j].name)+16` per param — but never counts `params[j].ty.name`, which is rendered via `": %s"` at line 122. Because `p += snprintf(...)` advances by the *would-be* length, once a long type name truncates, `p` passes `end`; the next `(size_t)(end - p)` underflows to ~2^64 and subsequent `snprintf` calls write attacker-controlled bytes past the heap allocation.
- **Trigger:** Open/edit any `.lat` via the LSP (`textDocument/didOpen`/`didChange` → `lsp_analyze_document` → `extract_symbols`) containing `fn f(p: AAAA…64+ chars) {}`. For a 64-char type, `siglen=50` but output needs 74 bytes. Confirmed under ASan: heap-buffer-overflow WRITE at line 124 into the 50-byte region. Fires even on partial parses; identifier length is uncapped (`lexer.c:77-80`).
- **Consequence:** Content- and offset-controlled heap OOB write (escalates with multiple long-typed params) reachable merely by opening a malicious source file in any editor using clat-lsp → memory corruption / potential RCE.
- **Suggested fix:** Add `strlen(params[j].ty.name)` to `siglen`. Better, replace the `p += snprintf` idiom everywhere with a bounded append helper that clamps `p` to `end` (so `end - p` can never go negative), or build the signature with a growable string builder.

#### C2. Heap overflow building impl-method signature — same under-sized buffer
- **Location:** `src/lsp_analysis.c:295-308` (malloc:297, overflowing snprintf:306/308)
- **Category:** OOB-write
- **Mechanism:** Identical to C1 in the `ITEM_IMPL` branch: `msiglen` omits `params[k].ty.name`; `p += snprintf` over-advances past `end`, `(size_t)(end - p)` underflows, later writes land out of bounds.
- **Trigger:** Open an impl block via the LSP with a long method-param type, e.g. `impl Foo for P { fn m(self: P, q: BBBB…80+ chars) {} }`. ASan: heap-buffer-overflow WRITE at line 308 into the region from line 297.
- **Consequence:** Same attacker-controlled heap corruption / potential RCE, reachable from untrusted source via didOpen/didChange.
- **Suggested fix:** Same as C1 — count the type-name length and use a clamped/bounded writer for both the function and impl-method signature builders.

---

### HIGH

#### H1. 32-bit wraparound in `Buffer.write_u16/u32/u64/i64` bounds check → controlled heap-underflow write (wasm32)
- **Location:** `src/builtin_methods.c:1214,1243,1333,1368` (StackVM); `src/regvm.c:2302,2331` (RegVM)
- **Category:** OOB-write
- **Mechanism:** Guard is `(size_t)args[0].as.int_val + N > buffer.len`. On wasm32 (`size_t`=32-bit), a positive 64-bit index like `0xFFFFFFFD` passes the `<0` test, truncates, and `+4` wraps to 1 (≤ len), bypassing the check; then `data[i]` with `i=0xFFFFFFFD` resolves via 32-bit pointer wrap to `data-3`.
- **Trigger:** `flux b = Buffer::new(16); b.write_u32(4294967293, 305419896)` via `lat_run_line` / `lat_run_line_regvm`. ASan on the emcc build: heap-buffer-overflow WRITE 3 bytes before a 16-byte region.
- **Consequence:** Attacker-controlled-byte write at an attacker-chosen small negative offset before any Buffer — corrupts adjacent allocations/allocator metadata inside wasm linear memory. 64-bit hosts unaffected (no truncation).
- **Suggested fix:** Validate without truncation: require `len >= N` then `idx >= 0 && (uint64_t)idx <= (uint64_t)len - N` (or compare in 64-bit before any `(size_t)` cast). Apply to all write_* widths in both VMs.

#### H2. 32-bit wraparound in `Buffer.read_*` bounds check → OOB read / heap disclosure (wasm32)
- **Location:** `src/builtin_methods.c:1203,1231,1271,1283,1295,1307,1319,1356`; `src/regvm.c:2287,2313,2357,2372,2387,2402`
- **Category:** OOB-read
- **Mechanism:** Same `(size_t)idx + N` truncate-then-wrap as H1, on the read path: `(size_t)0xFFFFFFF8 + 8` wraps to 0 ≤ len, then `memcpy(&v, data + i, N)` reads `data-8…`.
- **Trigger:** `flux b = Buffer::new(16); let x = b.read_u64(4294967288)` via the wasm entry points. read_u8/i8 (no `+N`) are correctly excluded.
- **Consequence:** OOB read of adjacent wasm-heap bytes returned to JS as Int/Float — leaks pointers/secrets, defeats in-module ASLR; pairs with H1 to form a read+write primitive. wasm32-only.
- **Suggested fix:** Same non-truncating bounds check as H1, applied to every read_* width in both VMs.

#### H3. `String.pad_left/pad_right`: 64-bit guard vs 32-bit-truncated malloc → giant OOB memset (wasm32)
- **Location:** `src/builtin_methods.c:903-916,918-930`; `src/regvm.c:1883-1899,1901-1917`
- **Category:** OOB-write
- **Mechanism:** Guard `(int64_t)slen >= n` compares full 64-bit `n`, but `malloc((size_t)n + 1)` and `plen = (size_t)n - slen` use the truncated value. On wasm32 `(size_t)4294967296 = 0`, so malloc(1) succeeds while `plen = 0 - 4 ≈ 4 GiB`, and `memset(buf, pad, plen)` overruns the 1-byte allocation.
- **Trigger:** `"abcd".pad_left(4294967296, "x")` (RegVM also single-arg) via the wasm entry points. ASan: `negative-size-param: (size=-4)` in `builtin_string_pad_left`. 64-bit hosts safe (malloc ~4 GiB fails → clone).
- **Consequence:** Reliable massive OOB heap write (heap corruption / wasm trap = DoS) from a single untrusted integer at the sandbox boundary; mirrored across both VMs and both pad directions.
- **Suggested fix:** Do the length math and guard in 64-bit before casting; reject `n` that exceeds a sane cap or won't fit in `size_t` (e.g. `if (n < 0 || (uint64_t)n > MAX_STR) return clone/err;`).

#### H4. Use-after-free: `s_varrefs` realloc invalidates the value pointer during variable-tree expansion
- **Location:** `src/dap.c:32-42, 222-271, 364-366`
- **Category:** uaf
- **Mechanism:** `handle_variables` calls `build_compound_variables(&ref->value)` where `ref = varrefs_find(...)` points *into* the global `s_varrefs` array. The loop re-reads `val->as.array.len`/`elems[i]` each iteration and, for each compound child, calls `get_varref_for_value → varrefs_add`, which `realloc(s_varrefs)` on growth — moving the buffer and dangling `val`. `varrefs_clear` only zeroes count, so the cap boundary is crossed mid-loop.
- **Trigger:** Under `--dap`, stop at a breakpoint and expand a compound value with ≳31 nested compound children (e.g. global `g = [[0],[1],…,[39]]`). ASan: heap-use-after-free READ in `build_compound_variables`, freed by the realloc in `varrefs_add`.
- **Consequence:** UAF read on the ordinary variables-display path; dangling `elems` dereferenced in `value_repr` → wild read / crash / info disclosure.
- **Suggested fix:** Don't alias into the growable array — copy `ref->value` into a local before iterating, pre-reserve `s_varrefs` capacity for the whole subtree, or re-fetch `ref` by index after each `varrefs_add`.

#### H5. Unbounded stack buffer overflow in REPL line accumulation (`strcat` into `accumulated[65536]`)
- **Location:** `src/main.c:601,612-613` (duplicated at 752/763-764 in `repl_regvm`, 889/900-901 in `repl_tree_walk`)
- **Category:** OOB-write
- **Mechanism:** Fixed 64 KiB stack buffer written by unbounded `strcat(accumulated, "\n")` / `strcat(accumulated, line)` with no length check. The default build links libedit, whose `readline()` returns unbounded lines. Two paths: (1) a single line >65535 bytes; (2) `input_is_complete()` only checks bracket depth, so unbalanced lines accumulate without reset.
- **Trigger:** `./clat` (or `./clat < script`); a 70000-char line crashed with exit 133 (stack-canary), 40000 unbalanced `{` lines also crashed. `CFLAGS` does not request `-fstack-protector`.
- **Consequence:** OOB stack write with attacker-controlled length/content. Guaranteed DoS on canary builds; classic return-address smash (potential code execution) where no canary is present. Pasting a large snippet is an ordinary user action.
- **Suggested fix:** Replace the fixed buffer with a dynamically grown string (realloc), bound each append, and cap total accumulated length; reject/trim oversized input. Fix all three REPL variants.

---

### MEDIUM

#### M1. `textDocument/didChange` with missing/non-string `uri` → `lsp_analyze_document(NULL)` crash
- **Location:** `src/lsp_server.c:547-557`
- **Category:** null-deref
- **Mechanism:** `lsp_json_str(td,"uri")` returns NULL for a missing/non-string uri; both `find_document(srv,NULL)` and `add_document(srv,NULL,...)` return NULL (their `if(!uri)` guards), so `doc` stays NULL and is passed to `lsp_analyze_document`, which dereferences `doc->diag_count` (`lsp_analysis.c:1014`). `handle_did_open` has `if(!doc) return;`; `handle_did_change` does not.
- **Trigger:** `{"method":"textDocument/didChange","params":{"textDocument":{},"contentChanges":[{"text":"..."}]}}`. Reproduced: SIGSEGV. No initialize handshake required.
- **Consequence:** Single malformed notification crashes the language server (DoS); editor loses all language features until restart.
- **Suggested fix:** Add `if (!doc) return;` after the find/add in `handle_did_change` (mirror `handle_did_open`), and/or NULL-guard `lsp_analyze_document`.

#### M2. `textDocument/didClose` with missing `uri` → `strcmp(doc->uri, NULL)` in `remove_document`
- **Location:** `src/lsp_server.c:353-360`
- **Category:** null-deref
- **Mechanism:** `remove_document` lacks the `if(!uri) return;` guard that `find_document`/`add_document` have, so `strcmp(documents[i]->uri, NULL)` dereferences NULL.
- **Trigger:** One valid `didOpen`, then a `didClose` whose `textDocument` has no `uri`. Reproduced: SIGSEGV. (Requires ≥1 open doc so the loop body runs.)
- **Consequence:** Single malformed `didClose` crashes the LSP server (DoS).
- **Suggested fix:** Add `if (!uri) return;` at the top of `remove_document`.

#### M3. Quadratic output blowup from accumulated indentation on nested braces (memory-exhaustion DoS)
- **Location:** `src/formatter.c:117` (`emit_indent`), `502-517` (`{` handler, `f.indent++`)
- **Category:** dos
- **Mechanism:** Each `{` increments `f.indent` and starts a new line; `emit_indent` emits `4*indent` spaces per line, so N braces → `2·N·(N-1)` = O(N²) output into a doubling `LatStr`. No input/output cap on any entry point.
- **Trigger:** Format a file of N `{` via LSP `textDocument/formatting` (`lsp_server.c:2576`), `clat fmt file.lat` (`main.c:1469`), or `clat fmt` stdin (`main.c:1430`). Measured exactly quadratic; ~200 KB of `{` → ~80 GB output.
- **Consequence:** Tiny input drives memory/CPU exhaustion; OOM-kills the CLI or wedges the long-lived LSP session (realistic via format-on-save / CI auto-format).
- **Suggested fix:** Clamp `f.indent` in `emit_indent` and/or impose an output-size ceiling; bound input size at the format entry points.

#### M4. Use-after-free: varref stores a non-owning shallow value that `evaluate` can free before later expansion
- **Location:** `src/dap.c:32-42, 136-141, 448-466, 638`
- **Category:** uaf
- **Mechanism:** `varrefs_add` stores a shallow `LatValue` copy whose `as.array.elems` aliases the live heap buffer ("do NOT free"). An intervening `evaluate` (`g = 0`) runs arbitrary code on the live VM → `env_set` → `value_free` on the old array (`env.c:151`). Varrefs persist for the whole stop, so re-expanding the stale varref dereferences freed `elems` in `value_repr`.
- **Trigger:** Under `--dap`: expand a compound global (stores varref), send `evaluate` that reassigns it, then send `variables` for that varref. ASan: UAF READ at `value.c:1370` from `dap.c:638`.
- **Consequence:** UAF read (crash / freed-heap info disclosure) via standard variables→evaluate→variables debug-console workflow.
- **Suggested fix:** Make varrefs own their data (deep-clone the value into the varref), or invalidate/refresh all outstanding varrefs after any side-effecting `evaluate`. (Shares root cause with H4.)

#### M5. NULL-deref when DAP request `command` is present but not a JSON string
- **Location:** `src/dap.c:487,490` (and identically `587,590`)
- **Category:** null-deref
- **Mechanism:** `const char *command = cmd ? cmd->valuestring : "";` — cJSON sets `valuestring=NULL` for non-string items, so `command` becomes NULL and the first `strcmp(command, ...)` dereferences NULL. The sibling `type` field and the disconnect loop (`dap.c:684`) are guarded correctly; these two sites are not.
- **Trigger:** `{"type":"request","seq":1,"command":123}` or `"command":null`. ASan: SEGV in `strcmp` from `dap_handshake`. Crashes at the first handshake request and at every stop.
- **Consequence:** Single malformed DAP message crashes the debug adapter (DoS).
- **Suggested fix:** Guard with `cmd && cmd->valuestring` (or `cJSON_IsString(cmd)`) at lines 487 and 587.

#### M6. Untrusted search path: CWD-relative `./extensions/` dlopen preferred over user/system dirs (library planting → RCE)
- **Location:** `src/ext.c:270-299`
- **Category:** other (CWE-426/427)
- **Mechanism:** The search list places the two `./extensions/<name>` CWD-relative paths *first*, before `~/.lattice/ext` and `$LATTICE_EXT_PATH`; the dlopen loop breaks on the first hit. Name validation only blocks `/` and `\` (traversal), not CWD planting. `RTLD_NOW` runs the library constructor at load.
- **Trigger:** Run any script calling `require_ext("<name>")` with CWD in an attacker-writable directory containing `./extensions/<name>.dylib`. PoC ran code in `__attribute__((constructor))` and returned attacker values; also shadows a trusted same-named install.
- **Consequence:** Arbitrary native code execution in the host process (preconditions: attacker-writable CWD + victim runs a `require_ext` script there).
- **Suggested fix:** Drop or de-prioritize the CWD search (trusted dirs first), require explicit opt-in for project-local extensions, and resolve to absolute paths.

---

### LOW

#### L1. Identifiers ≥128 chars are split with an injected space, silently corrupting reformatted source
- **Location:** `src/formatter.c:468-469,492` (root in `read_word` at 172-178)
- **Category:** logic
- **Mechanism:** `read_word` truncates to 127 chars into `word[128]`; the emit loop advances only `strlen(word)` source positions, leaving the remainder to be re-scanned as a fresh word, and `f.last==LAST_IDENT` injects a separating space.
- **Trigger:** Format source with a ≥128-char identifier (e.g. `let <300 a's> = 1`) via `clat fmt`/`--stdin`/LSP formatting. Reproduced: rewritten as three space-separated chunks; formatted output fails to parse.
- **Consequence:** Formatter silently produces non-equivalent/invalid code; spurious `--check` mismatch. No memory-safety impact; pathological trigger.
- **Suggested fix:** Consume the full identifier (loop until ident chars exhausted, or grow the buffer) rather than truncating to 127.

#### L2. Unchecked `realloc` results dereferenced (NULL-write + leak) throughout analysis builders
- **Location:** `src/lsp_analysis.c:153-154` (and 211-212, 315-316, 330-331, 457-458, 477-479, 773, 814, 830, 842, 961)
- **Category:** null-deref
- **Mechanism:** `ptr = realloc(ptr, n)` overwrites the original pointer with no NULL check, then immediately does an indexed write. On allocator failure → leak + NULL-region write. Notably the `malloc` sites in the same file *are* NULL-checked, so this is an isolated oversight.
- **Trigger:** A very large untrusted document that exhausts the heap so a realloc returns NULL.
- **Consequence:** Crash (DoS) under memory pressure plus a leak; OOM-gated, no exploitable corruption.
- **Suggested fix:** Use a temp pointer + NULL check at each site (free old on failure), matching the existing malloc handling.

#### L3. Unbounded `Content-Length`: huge malloc + blocking `fread` wedges the single-threaded server
- **Location:** `src/lsp_protocol.c:19-25`
- **Category:** dos
- **Mechanism:** `content_length = atoi(...)`, only validated `<= 0`; then `malloc(content_length+1)` (up to ~2 GB) and a blocking `fread` of the full declared length. The server processes one message per loop iteration.
- **Trigger:** `Content-Length: 2000000000\r\n\r\n` then stall. Confirmed: server blocks in `fread`, serving nothing further; streaming the body buffers ~2 GB.
- **Consequence:** DoS / memory exhaustion. No corruption (alloc/fread/NUL are internally consistent). Bounded by the trusted-pipe trust model.
- **Suggested fix:** Impose a sane upper cap on `content_length`; consider timeout/non-blocking reads.

#### L4. Unescaped source-derived type/param fields in JSON doc output → JSON injection / malformed JSON
- **Location:** `src/doc_gen.c:1197,1652,1669,1688,1709,1721`
- **Category:** injection
- **Mechanism:** These fields are written with raw `sb_printf("%s", ...)` instead of `sb_append_json_str` (which *is* used for the function `return_type` at line 1641). The readers stop only at structural delimiters, so `"` and `\` (and `,`/`:`) pass through verbatim.
- **Trigger:** `clat doc --json` on a file with `struct S { x: a"b\c }`, `enum E { V(a"b) }`, etc. Reproduced malformed JSON; a crafted type like `string", "injected_key": "...` injects sibling keys (valid-but-attacker-shaped JSON).
- **Consequence:** Breaks or structurally injects into JSON consumed by downstream editor/LSP/CI tooling. No memory unsafety.
- **Suggested fix:** Route all six fields through `sb_append_json_str`, as `return_type` already is.

#### L5. `base64_decode` is non-strict: unchecked trailing padding bits and mid-group `=` accepted
- **Location:** `src/crypto_ops.c:663-685` (reachable via `eval.c:5957`, `runtime.c:1430`)
- **Category:** crypto-misuse *(majority 2/3 confirmed; one refuter argued RFC-4648-permitted leniency, not-a-bug)*
- **Mechanism:** Padded symbols are zero-filled and emitted without verifying the last data symbol's unused low bits are zero; the validity guard only forces symbols 1–2 non-pad, permitting `=` in position 3 with data in position 4.
- **Trigger:** `base64_decode("Zh==")` and `"Zg=="` both → `0x66`; `"Zm9="` and `"Zm8="` both → `"fo"`; `"AB=C"` → 2 bytes.
- **Consequence:** Malleable/non-canonical decoding; only matters where a downstream consumer assumes a 1:1 encoding (no such consumer exists in-tree). No memory safety impact.
- **Suggested fix:** If strictness is desired, verify padding-bit zeroing and reject mid-group `=`; otherwise document the leniency explicitly.

#### L6. OpenSSL digest path ignores EVP return codes → silent empty/incorrect hash on failure
- **Location:** `src/crypto_ops.c:39-91`
- **Category:** crypto-misuse
- **Mechanism:** `crypto_sha256/md5/sha512` ignore `EVP_DigestInit_ex/Update/Final` return values; `hash_len` stays 0 on failure, `hex_encode(hash,0)` returns `""`, and `*err` is left unset (only the `!ctx` alloc branch sets it). Callers treat `cerr==NULL` + `""` as a valid digest.
- **Trigger:** Algorithm/provider unavailable (e.g. MD5 under a FIPS provider) or OOM mid-digest.
- **Consequence:** Digest failure reported as success with a constant empty hash; integrity/content-addressing (`package.c:274`) collapses. Environmental trigger; no memory unsafety. (Note: the "`(void)err` disables the channel" framing is slightly overstated — the channel is live only for the ctx-alloc path.)
- **Suggested fix:** Check each EVP return code; on failure set `*err` and return NULL.

#### L7. Per-function memory leak of duplicated param name in `ext_load`
- **Location:** `src/ext.c:337-346`
- **Category:** leak
- **Mechanism:** `pname = strdup("args")` is stored in `param_names[0]` and passed to `value_closure`, which *copies* each name (`value.c:293-294`) rather than taking ownership. Only `free(param_names)` (the array) runs, never `pname`.
- **Trigger:** Every successful uncached `require_ext()`, once per registered function.
- **Consequence:** Small unconditional heap leak (~one `strdup("args")` per exported fn per load). The malloc-failure early-return at line 339 also leaks `pname` + partial map (OOM-only).
- **Suggested fix:** `free(pname)` after `value_closure` (or skip the strdup since the names are copied and the array is freed).

---

## 3. Residual Coverage

**Now covered (this pass):**
- **LSP:** `didOpen`/`didChange`/`didClose`/`formatting`, symbol/folding/semantic-token extraction, signature builders, the JSON-RPC framing/transport (`lsp_protocol.c`), and growable-array append patterns.
- **Formatter:** indentation/brace handling, identifier word-reading, all three entry points (`fmt` file, `--stdin`, LSP formatting).
- **Doc-gen:** the `--json` rendering path and its escaping.
- **Crypto:** OpenSSL digest wrappers (sha256/md5/sha512) and `base64_decode`.
- **WASM boundary:** `lat_run_line` / `lat_run_line_regvm`, Buffer read/write builtins (both VMs), `String.pad_*` — with the wasm32 vs 64-bit `size_t` distinction characterized.
- **Debugger/DAP:** the `variables`/`evaluate` flow, varref lifetime, and request-`command` parsing.
- **CLI/file-load:** REPL line accumulation, `read_file`, and the `fmt`/`doc` command dispatch.
- **FFI:** the `require_ext` → `ext_load` dlopen search path and closure construction.

**Still un-reviewed or only shallowly touched:**
- The remaining LSP request handlers not implicated above (completion, hover, definition, rename, code actions) and their own buffer/JSON handling.
- The **Windows/Schannel crypto path** and **MEMORY64 wasm builds** (the wasm32 findings explicitly do not apply to a 64-bit wasm target; behavior there is untested here).
- The **package manager** beyond checksum computation — registry fetch, archive extraction, `lattice.lock` parsing, and network I/O.
- The HMAC/AEAD/random portions of the crypto module (only the digest and base64 paths were examined).
- The doc-gen **non-JSON** (Markdown/HTML) renderers.

No findings are asserted beyond the confirmed JSON; the residual list above is scope guidance, not implied vulnerabilities.