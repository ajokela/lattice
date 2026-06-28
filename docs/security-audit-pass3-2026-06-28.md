# Lattice C Interpreter/VM — Third Adversarial Security Pass (Residual Coverage)

## 1. Executive Summary

This third pass targeted the residual attack surface left after passes 1 and 2: the remaining LSP handlers, the package fetch/extract/lockfile and module-resolution path, HMAC/AEAD/RNG crypto, TLS cert/hostname/Schannel validation, HTTP/net request and response parsing, and the doc-gen markdown/html renderers. Eight findings survived three independent adversarial refuters each (majority-confirmed).

**Counts by severity (as confirmed):**

| Severity | Count |
|----------|-------|
| Critical | 0 |
| High     | 1 |
| Medium   | 1 |
| Low      | 6 |
| **Total**| **8** |

**Posture.** No memory-corruption, use-after-free, OOB-write, or RCE-class defects surfaced on this surface. The single High is a supply-chain module-path traversal (`[package] entry` not validated, unlike every sibling dependency field); the Medium is textbook client-side CRLF/header injection in the HTTP request builder. The remaining six are low-impact hardening/correctness items: an OOM-gated null-deref, two OOM-gated resource leaks, an unbounded-response DoS, a NUL-truncation data-integrity bug on binary downloads, an LSP code-action edit-truncation, and an unescaped-markdown injection. Five of the eight (ranks 3–6, plus the entry-traversal's escalation leg) are gated behind preconditions an attacker on the relevant surface already controls (OOM, or an already-installed malicious package), which caps practical impact.

**Comparison to passes 1 and 2.** Passes 1 and 2 exercised the core execution machinery — bytecode/reg VM dispatch, compiler, evaluator, parser, and the bump-arena/region memory model — where the highest-value, memory-safety-class issues concentrate. This pass deliberately swept the periphery, and the result is a lighter, shallower tail: zero critical/memory-corruption findings here, with the only genuinely cross-boundary issues being the package-manager entry traversal and HTTP CRLF injection. The pattern across all three renderers/validators in this pass is *consistency gaps* — the codebase already does the right thing in adjacent code (validates every other package field; escapes JSON and HTML; caps `net_tcp_read_bytes`) and simply missed one sibling — which is characteristic of a maturing codebase whose remaining defects are omissions rather than systemic flaws.

---

## 2. Findings (Critical → Low)

### High

#### F1 — Unvalidated `[package] entry` enables module-path traversal out of `lat_modules/`
- **Location:** `src/package.c:644` (unvalidated source); `src/package.c:1966-1975` (sink)
- **Category:** path-traversal
- **Mechanism:** `out->meta.entry = safe_strdup(map_get_str(pkg, "entry"))` is read with no validation. The validator `pkg_path_component_is_safe()` (`:85-93`, rejects `..`, `/`, `\`, control bytes) is applied to dependency `version` and git refs at `:699-701` but **never** to `meta.entry` — a direct asymmetry. In `pkg_resolve_module` Strategy 3, when a package lacks `main.lat` and `src/main.lat`, the resolver builds `lat_modules/<name>/<entry>` verbatim and calls `realpath()` with no containment/prefix check, returning the resolved path even if it escaped `lat_modules/`. The guard at `:1932` checks the import *name*, not the entry field.
- **Trigger:** A malicious package installed into `lat_modules/<name>/` ships a `lattice.toml` with `entry = "../../../../../../tmp/evil.lat"` and omits both `main.lat` and `src/main.lat`. On `import <name>`, Strategy 3 resolves and returns the out-of-tree absolute path, which the import loader (`stackvm.c:7785-7826`, `eval.c:10345`, `regvm.c:5632`) reads, lexes, parses, and executes.
- **Consequence:** Import resolution escapes the package sandbox to anywhere on the filesystem (arbitrary file read consumed as source; code execution if a readable `.lat` exists at the target). **Confirmed nuance from verifiers:** the trigger presupposes an already-installed, already-imported malicious package, which on import already runs with full host privileges and can call `read_file()` directly — so the traversal adds little *new* capability and is primarily a containment/defense-in-depth gap rather than a fresh privilege boundary crossing (verifier consensus rated the practical impact low–medium). It remains High-ranked as the only cross-trust-boundary resolution defect and should be fixed to match the validation already present on adjacent fields.
- **Suggested fix:** Apply `pkg_path_component_is_safe()` to `meta.entry` at read time (`:644`), rejecting `..`, separators, and control bytes — exactly as done for sibling dependency fields. Additionally (defense in depth) confine the `realpath()` result in Strategy 3 to the canonicalized `lat_modules/<name>/` prefix via `strncmp`, returning `NULL` on escape.

### Medium

#### F2 — No CRLF/control-char sanitization in request line or header values → HTTP request splitting/smuggling
- **Location:** `src/http.c:105,117` (wire formatting); `src/http.c:67-79` (`http_parse_url`)
- **Category:** injection (CWE-93 / CWE-113)
- **Mechanism:** `http_parse_url` copies everything after the host verbatim into `url->path` (`out->path = strdup(slash ? slash : "/")`, `:79`) and validates only scheme/port — never CR/LF. `format_request` then writes `"%s %s HTTP/1.1\r\n"` (method, path) at `:105` and `"%s: %s\r\n"` (header key, value) at `:117` with bare `%s` and literal `\r\n` separators. Embedded `0x0D`/`0x0A` are not NUL, so they pass through verbatim; `cap` is computed from real `strlen()`, so no truncation incidentally blocks them. `net_tcp_write`/`net_tls_write` transmit raw bytes unchanged.
- **Trigger:** A program interpolates untrusted data into a request field, e.g. `http_get("https://api.example.com/x?q=" + user_input)` with `user_input = "foo\r\nX-Injected: 1"`, or an attacker-influenced `options.headers` value/method via `http_post`/`http_request`. Builtins pass URL, method, and header keys/values into `HttpRequest` unfiltered (`eval.c:4147-4170,4225-4251`; `runtime.c` mirror).
- **Consequence:** Header injection, `Host` override, cache poisoning, and request smuggling (a second request) to the upstream server or a shared proxy/cache. Impact is on the downstream peer and depends on the app routing untrusted data into a request field; no in-process memory unsafety.
- **Suggested fix:** Reject (or fail the request on) any CR/LF or control byte in method, path/query, host, and header keys/values — e.g. `strpbrk(field, "\r\n")` → error in `http_parse_url` and `format_request`. Constrain header keys to the HTTP token charset, mirroring curl / Go `net/http` / Python `http.client`.

### Low

#### F3 — Undersized buffer truncates the "Wrap in try/catch" code-action edit for deeply-indented lines
- **Location:** `src/lsp_server.c:3003-3007`
- **Category:** logic / correctness
- **Mechanism:** `new_len = indent_len * 3 + strlen(original) + 64`, but the format string substitutes `indent` **5** times plus 42 literal bytes (+NUL = 43), so required size is `indent_len * 5 + strlen(original) + 43`. `allocated − required = 21 − 2*indent_len`, which goes negative at `indent_len >= 11`.
- **Trigger:** A `textDocument/codeAction` whose client-controlled `context.diagnostics` message matches the try/catch trigger words ("throw"/"exception"/"uncaught"/"Unhandled error") and whose target line has ≥11 leading whitespace bytes (≈3 levels of 4-space indent). No clamp on `indent_len`.
- **Consequence:** `snprintf` silently truncates the `WorkspaceEdit` newText (drops the closing `}` / catch block), so applying the quick fix yields unbalanced source. **Bounded by `new_len` — no heap overflow or OOB write**, and the return value is not used to advance any pointer. Correctness-only.
- **Suggested fix:** Size the allocation from the actual format (`indent_len * 5 + strlen(original) + 64`), or clamp `indent_len`; optionally check the `snprintf` return against `new_len`.

#### F4 — Unchecked `format_request()` return dereferenced via `strlen()` before send
- **Location:** `src/http.c:332-337`
- **Category:** null-deref
- **Mechanism:** `format_request()` returns `NULL` on `malloc(cap)` failure (`:100-101`). `http_execute()` stores it in `raw_req` and immediately passes `strlen(raw_req)` to `net_tls_write`/`net_tcp_write` with no NULL check; `strlen` is evaluated as the argument, before any callee can inspect the pointer. Inconsistent with the surrounding defensive style (`resp_buf` malloc *is* checked at `:351-352`).
- **Trigger:** OOM only — `malloc` failure is the sole `NULL`-producing path. Not attacker/remote-controllable; `cap` and all fields are program-supplied.
- **Consequence:** `strlen(NULL)` crash/DoS; also bypasses the cleanup at `:341-345` (leaks the open `fd` and parsed `HttpUrl`), though moot on crash.
- **Suggested fix:** `if (!raw_req) { net_tcp_close/net_tls_close(fd); http_url_free(&url); return NULL; }` immediately after `format_request`.

#### F5 — Socket fd and parsed URL leaked when response-buffer allocation fails
- **Location:** `src/http.c:351-352`
- **Category:** leak
- **Mechanism:** After a successful connect/write, `char *resp_buf = malloc(resp_cap); if (!resp_buf) return NULL;` returns without `net_tcp_close`/`net_tls_close(fd)` or `http_url_free(&url)`. Every other error path in the function (`:341-346`, `:368-372`, normal exit) performs this cleanup — a genuine asymmetry. The same omission appears in `parse_response`'s OOM returns (`:173,175,233,285`), which leak the partial `HttpResponse` and its header arrays.
- **Trigger:** Allocator failure on the fixed 8 KiB response buffer (and tiny fixed/`body_len`-bounded allocations in `parse_response`). Response *content* cannot drive these failures — the chunked path already clamps to available bytes — so untrusted input does not control the trigger.
- **Consequence:** One leaked fd plus a few small heap strings per occurrence, at the moment the process is already OOM. OOM-gated; not amplifiable by a remote party.
- **Suggested fix:** Route all error returns through a single `goto cleanup:` that closes `fd`, frees `url`, and frees `resp_buf`/header arrays; add the missing cleanup to the `parse_response` OOM returns.

#### F6 — No upper bound on total HTTP response size read into memory
- **Location:** `src/http.c:380-390`
- **Category:** dos
- **Mechanism:** `http_execute` accumulates the full response into `resp_buf` via an unbounded doubling-realloc loop. The only exit conditions are read error, EOF (`clen==0`), or realloc/overflow failure — no maximum-size ceiling. The single `SO_RCVTIMEO` (`net.c:331`) is a *per-recv* idle timeout, not a total deadline, so a server dripping ≥1 byte per window streams indefinitely while `resp_buf` doubles toward OOM. Inconsistent with `net_tcp_read_bytes` (which has `NET_READ_BYTES_MAX = 256 MiB`) and the chunked decoder (which clamps to available bytes).
- **Trigger:** Any malicious/compromised/MITM'd server reached via `http_get`/`http_post`/`http_request`, or the package registry via `registry_fetch_versions` (`package.c:306`) / `registry_download_package` (`package.c:408`).
- **Consequence:** Memory-exhaustion DoS of the client process (the realloc-failure path breaks gracefully, so on a host with a per-process limit it may error rather than crash the host). No memory corruption; victim must initiate the request.
- **Suggested fix:** Enforce a maximum total response size (e.g. reuse `NET_READ_BYTES_MAX`), aborting when `resp_len` exceeds the ceiling; optionally honor `Content-Length` as an early bound.

#### F7 — Binary response bodies silently truncated at first NUL byte (corrupts archive/tarball downloads)
- **Location:** `src/http.c:374` (with `net.c:207-214`, `tls.c:119-126`)
- **Category:** logic / data-integrity
- **Mechanism:** `net_tcp_read`/`net_tls_read` NUL-terminate (`buf[n]='\0'`) and return only a `char *`, discarding the recv length `n`. `http_execute` recovers length via `size_t clen = strlen(chunk)`, so any `0x00` in a recv block undercounts it — post-NUL bytes are never copied into `resp_buf`. Worse, a chunk *beginning* with NUL yields `clen==0`, which `:375-378` treats as EOF, terminating the whole read. `parse_response`'s exact `body_len` is computed from the already-truncated buffer.
- **Trigger:** Any response body containing `0x00` (i.e. essentially all gzip/tar/zip archives). `registry_download_package` (`package.c:408,460`) writes `resp->body` to disk as package source; `builtin_write_file`'s `fputs`/text-mode is additionally NUL-unsafe.
- **Consequence:** Silent corruption/truncation of binary downloads and any non-text body. Data-integrity only; not memory-unsafe (failures surface loudly downstream as a broken archive/compile).
- **Suggested fix:** Propagate the recv byte count from `net_tcp_read`/`net_tls_read` (return length or out-param; `net_tcp_read_bytes` already tracks `total`) and `memcpy` exactly `n` bytes; represent the HTTP body as a length-delimited byte buffer rather than a C string end-to-end (including the write-to-disk path).

#### F8 — Markdown renderer emits source-derived names/types/doc-comments with no escaping (markdown injection; XSS when rendered to HTML)
- **Location:** `src/doc_gen.c:1492` (struct-field table); also `:1436,1468,1483,1509,1517-1520,1537,1549,1564,1578,1594,1597`
- **Category:** injection
- **Mechanism:** `render_markdown` interpolates source strings verbatim, e.g. `sb_printf(sb, "| `%s` | `%s` | %s |\n", f->name, f->type_name?:"", f->doc?:"")`. `ds_read_doc_block` (`:128-170`) joins multi-line `///` blocks with embedded `\n` and copies arbitrary bytes; field/return types are read free-form (`parse_struct_fields :276-281`, terminating only on `,`/`}`/`\n`). So `|`, backticks, embedded newlines, and raw HTML pass through. This is an asymmetry: `render_json` routes through `sb_append_json_str` (`:1216-1241`) and `render_html` through `sb_append_html_esc` (`:1244-1254`), but `render_markdown` escapes nothing.
- **Trigger:** `clat doc file.lat` (Markdown is the default format, `:2184`) on an untrusted `.lat` source — e.g. a struct field with a multi-line doc, a type containing `|`, or `/// <img src=x onerror=alert(1)>`. With `-o`, the output is persisted as `.md` (`:2342-2346`), satisfying the stored-XSS precondition.
- **Consequence:** Deterministic table-row corruption (the unconditional bug, independent of any downstream) plus markdown injection; raw HTML emitted verbatim becomes stored XSS if the `.md` is later rendered by a raw-HTML-permitting, non-sanitizing processor (MkDocs, Pandoc, `marked` without sanitize). Developer-facing tool; the XSS leg is conditional on a downstream chain outside this codebase.
- **Suggested fix:** Add a markdown escaper (escape `|`, backtick, leading block markup, and HTML-special characters, or strip raw HTML) and route all source-derived names/types/docs through it in `render_markdown`, mirroring the JSON and HTML renderers — at minimum for the structural table cells at `:1492`.

---

## 3. Residual Coverage Note

**Now covered by this pass.** Package manager: registry fetch (`registry_fetch_versions`/`registry_download_package`), archive write-to-disk, lockfile/manifest parsing and validation (`pkg_manifest_parse`, `pkg_path_component_is_safe`), and module resolution Strategies 1–3. HTTP/net: URL parsing, request formatting, the response read loop, chunked/plain body decode, and the `net`/`tls` read/write primitives. LSP: the code-action handlers (`handle_code_action`, try/catch and related quick-fix edit construction). Doc-gen: all three renderers (markdown/html/json) and the doc-comment/type extraction (`ds_read_doc_block`, `parse_struct_fields`).

**Clean on this pass (no confirmed findings).** The HMAC/AEAD/RNG crypto and the TLS certificate/hostname/Schannel validation paths produced no majority-confirmed defects in this set. That is a positive signal but not equivalent to a proof of correctness on this run.

**Still warrants focused review.** Because crypto and certificate/hostname verification are the highest-consequence surfaces (a flaw there is silent and security-critical, unlike the loud failures dominating this pass's Low tail), a dedicated targeted review remains the highest-value next step — specifically: constant-time comparison in HMAC/tag verification and key handling; AEAD nonce-uniqueness and tag-check ordering; RNG seeding/entropy source; and TLS certificate-chain construction, hostname/SAN matching, and the Schannel path's parity with the OpenSSL path. Two systemic patterns this pass surfaced are also worth a sweep beyond the cited sites: (1) the C-string/`strlen` body model throughout the HTTP stack (F7) — any other place that lengths binary data via `strlen` shares the truncation defect; and (2) renderer/validator *consistency* — the codebase repeatedly does the right thing in one of N sibling paths and misses one (entry vs. other package fields in F1; markdown vs. json/html in F8), so an explicit "validate/escape every sibling" audit would likely retire a class of latent issues rather than individual instances.