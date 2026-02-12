# Lattice

A crystallization-based programming language implemented in C, where data transitions between fluid (mutable) and crystal (immutable) states through a unique phase system.

## Overview

Lattice is an interpreted programming language built around the metaphor of crystallization. Values begin in a **fluid** state where they can be freely modified, then **freeze** into an immutable **crystal** state for safe sharing and long-term storage. This phase system gives you explicit, fine-grained control over mutability — rather than relying on convention, the language enforces it.

The language features a familiar C-like syntax with modern conveniences: first-class closures, structs with callable fields, expression-based control flow, pattern-style `for..in` iteration, try/catch error handling, structured concurrency with channels, and a self-hosted REPL written in Lattice itself.

Lattice compiles and runs on macOS and Linux with no dependencies beyond a C11 compiler and libedit. Optional features like TLS networking and cryptographic hashing are available when OpenSSL is present.

## Quick Start

**Prerequisites:** C compiler with C11 support, libedit (ships with macOS; `libedit-dev` on Debian/Ubuntu)

**Optional:** OpenSSL for TLS networking and crypto builtins (`openssl` via pkg-config)

```sh
make                          # build the interpreter
./clat examples/fibonacci.lat # run an example program
./clat                        # start the REPL
```

## Language Guide

### Variables & Phases

Lattice has three variable declaration keywords that control mutability:

```lattice
flux counter = 0      // fluid — mutable
fix  PI = freeze(3.14) // crystal — immutable
let  name = "Lattice" // inferred phase from the value
```

Phase transitions:

```lattice
flux data = [1, 2, 3]
fix frozen = freeze(data)   // fluid → crystal
flux thawed = thaw(frozen)  // crystal → fluid (copy)
flux copy = clone(data)     // independent deep copy
```

### Types

| Type | Description |
|------|-------------|
| `Int` | 64-bit signed integer |
| `Float` | 64-bit double-precision float |
| `Bool` | `true` or `false` |
| `String` | Immutable UTF-8 string |
| `Array` | Ordered, growable collection (`[1, 2, 3]`) |
| `Map` | Key-value hash map (created with `Map::new()`) |
| `Channel` | Thread-safe communication channel (created with `Channel::new()`) |
| `Struct` | Named record type with typed fields |
| `Closure` | First-class function value (`\|x\| x + 1`) |
| `Range` | Integer range (`0..10`) |
| `Unit` | Absence of a value (like void/nil) |

### Functions

```lattice
fn add(a: Int, b: Int) -> Int {
    return a + b
}

fn greet(name: String) -> String {
    "Hello, " + name + "!"  // implicit return
}
```

### Control Flow

**if/else** — expression-based, returns a value:
```lattice
let status = if score >= 90 { "A" } else { "B" }
```

**while:**
```lattice
flux i = 0
while i < 10 {
    i += 1
}
```

**for..in** — iterates over arrays and ranges:
```lattice
for item in [1, 2, 3] {
    print(item)
}

for i in 0..5 {
    print(i)  // 0, 1, 2, 3, 4
}
```

**loop** — infinite loop, use `break` to exit:
```lattice
loop {
    let line = input("> ")
    if typeof(line) == "Unit" { break }
    print(line)
}
```

`break`, `continue`, and `return` work as expected.

### Closures & Block Expressions

Single-expression closures:
```lattice
let double = |x| x * 2
let add = |a, b| a + b
```

Block-body closures:
```lattice
let process = |x| {
    let y = x * 2
    y + 1
}
```

Standalone block expressions:
```lattice
let result = {
    let a = 10
    let b = 20
    a + b
}
// result = 30
```

### Structs

```lattice
struct Point {
    x: Int,
    y: Int
}

let p = Point { x: 10, y: 20 }
print(p.x)  // 10
```

Structs support callable fields with a `self` parameter:

```lattice
struct Counter {
    count: Int,
    increment: Fn
}

let c = Counter {
    count: 0,
    increment: |self| self.count + 1
}
print(c.increment())  // 1
```

### Error Handling

```lattice
let result = try {
    let data = read_file("config.txt")
    data
} catch e {
    print("error: " + e)
    "default"
}
```

### Forge Blocks

`forge` blocks provide a controlled scope for building immutable values — mutate freely inside, freeze at the end:

```lattice
fix config = forge {
    flux temp = Map::new()
    temp.set("host", "localhost")
    temp.set("port", "8080")
    freeze(temp)
}
// config is now crystal (immutable)
```

### Concurrency

Lattice provides structured concurrency with `scope` blocks, `spawn` tasks, and channels. Each spawned task runs on its own thread with an independent garbage collector. Only crystal (frozen) values can be sent across channels — the phase system guarantees data-race safety at the language level.

**Channels** are created with `Channel::new()` and support `.send()`, `.recv()`, and `.close()`:

```lattice
let ch = Channel::new()
ch.send(freeze(42))
let val = ch.recv()  // 42
```

**Scope blocks** provide structured join semantics — all spawned tasks must complete before the scope exits:

```lattice
let ch1 = Channel::new()
let ch2 = Channel::new()
scope {
    spawn { ch1.send(freeze(compute_a())) }
    spawn { ch2.send(freeze(compute_b())) }
}
// both tasks are done here
let a = ch1.recv()
let b = ch2.recv()
```

Key rules:
- `spawn` inside a `scope` runs on a new thread; `spawn` outside runs synchronously
- `.send()` requires the value to be crystal (frozen) or a primitive type (Int, Float, Bool, Unit)
- `.recv()` blocks until a value is available, or returns Unit if the channel is closed and empty
- Channels cannot be frozen
- Errors in spawned tasks propagate to the parent scope

### Strict Mode

Enable `#mode strict` at the top of a file for stricter phase enforcement. In strict mode, `freeze()` on an identifier **consumes** the binding — the original variable is no longer accessible after freezing:

```lattice
#mode strict

flux data = [1, 2, 3]
fix frozen = freeze(data)
// data is no longer accessible here
```

## Standard Library

### Core Builtins

| Function | Signature | Description |
|----------|-----------|-------------|
| `print` | `print(args...)` | Print values to stdout with newline |
| `eprint` | `eprint(args...)` | Print values to stderr with newline |
| `print_raw` | `print_raw(args...)` | Print values to stdout without newline |
| `input` | `input(prompt?) -> String\|Unit` | Read a line from stdin (returns Unit on EOF) |
| `typeof` | `typeof(val) -> String` | Returns the type name of a value |
| `phase_of` | `phase_of(val) -> String` | Returns `"fluid"`, `"crystal"`, or `"unphased"` |
| `to_string` | `to_string(val) -> String` | Convert any value to its string representation |
| `len` | `len(val) -> Int` | Length of a string, array, or map |
| `exit` | `exit(code?)` | Exit the process with a status code (default 0) |
| `version` | `version() -> String` | Returns the Lattice version string |

### Phase Transitions

| Function | Signature | Description |
|----------|-----------|-------------|
| `freeze` | `freeze(val) -> crystal` | Transition a value to crystal (immutable) phase |
| `thaw` | `thaw(val) -> fluid` | Create a mutable copy of a crystal value |
| `clone` | `clone(val) -> Value` | Deep-clone a value |

### Type Constructors

| Function | Signature | Description |
|----------|-----------|-------------|
| `Map::new` | `Map::new() -> Map` | Create an empty map |
| `Channel::new` | `Channel::new() -> Channel` | Create a new channel for inter-thread communication |

### Type Conversion

| Function | Signature | Description |
|----------|-----------|-------------|
| `to_int` | `to_int(val) -> Int` | Convert Int, Float, Bool, or String to Int |
| `to_float` | `to_float(val) -> Float` | Convert Int, Float, Bool, or String to Float |
| `parse_int` | `parse_int(s) -> Int` | Parse a string as an integer |
| `parse_float` | `parse_float(s) -> Float` | Parse a string as a float |
| `ord` | `ord(ch) -> Int` | ASCII code of the first character |
| `chr` | `chr(code) -> String` | Character from an ASCII code |

### Math

| Function | Signature | Description |
|----------|-----------|-------------|
| `abs` | `abs(n) -> Int\|Float` | Absolute value (preserves type) |
| `floor` | `floor(n) -> Int` | Floor to integer |
| `ceil` | `ceil(n) -> Int` | Ceiling to integer |
| `round` | `round(n) -> Int` | Round to nearest integer |
| `sqrt` | `sqrt(n) -> Float` | Square root |
| `pow` | `pow(base, exp) -> Int\|Float` | Exponentiation |
| `min` | `min(a, b) -> Int\|Float` | Minimum of two numbers |
| `max` | `max(a, b) -> Int\|Float` | Maximum of two numbers |
| `random` | `random() -> Float` | Random float in [0, 1) |
| `random_int` | `random_int(low, high) -> Int` | Random integer in [low, high] inclusive |

### String Formatting & Regex

| Function | Signature | Description |
|----------|-----------|-------------|
| `format` | `format(fmt, args...) -> String` | Format string with `{}` placeholders |
| `regex_match` | `regex_match(pattern, str) -> Bool` | Test if pattern matches string (POSIX extended) |
| `regex_find_all` | `regex_find_all(pattern, str) -> Array` | Find all non-overlapping matches |
| `regex_replace` | `regex_replace(pattern, str, repl) -> String` | Replace all matches |

### JSON

| Function | Signature | Description |
|----------|-----------|-------------|
| `json_parse` | `json_parse(str) -> Value` | Parse JSON string into a Lattice value |
| `json_stringify` | `json_stringify(val) -> String` | Serialize a Lattice value to JSON |

### File System

| Function | Signature | Description |
|----------|-----------|-------------|
| `read_file` | `read_file(path) -> String` | Read entire file contents |
| `write_file` | `write_file(path, content) -> Bool` | Write string to a file |
| `append_file` | `append_file(path, content) -> Bool` | Append string to a file |
| `file_exists` | `file_exists(path) -> Bool` | Check if a file exists |
| `delete_file` | `delete_file(path) -> Bool` | Delete a file |
| `list_dir` | `list_dir(path) -> Array` | List directory contents |

### Path Utilities

| Function | Signature | Description |
|----------|-----------|-------------|
| `path_join` | `path_join(parts...) -> String` | Join path components with `/` |
| `path_dir` | `path_dir(path) -> String` | Directory portion of a path |
| `path_base` | `path_base(path) -> String` | Base name of a path |
| `path_ext` | `path_ext(path) -> String` | File extension (including `.`) |

### Environment & Process

| Function | Signature | Description |
|----------|-----------|-------------|
| `env` | `env(name) -> String\|Unit` | Get environment variable (Unit if not set) |
| `env_set` | `env_set(name, value)` | Set environment variable |
| `time` | `time() -> Int` | Current time in milliseconds since epoch |
| `sleep` | `sleep(ms)` | Sleep for milliseconds |

### Date & Time Formatting

| Function | Signature | Description |
|----------|-----------|-------------|
| `time_format` | `time_format(epoch_ms, fmt) -> String` | Format timestamp with strftime codes |
| `time_parse` | `time_parse(str, fmt) -> Int` | Parse datetime string to epoch milliseconds |

### Cryptography (requires OpenSSL)

| Function | Signature | Description |
|----------|-----------|-------------|
| `sha256` | `sha256(s) -> String` | SHA-256 hash as hex string |
| `md5` | `md5(s) -> String` | MD5 hash as hex string |
| `base64_encode` | `base64_encode(s) -> String` | Base64 encode |
| `base64_decode` | `base64_decode(s) -> String` | Base64 decode |

### Networking (TCP)

| Function | Signature | Description |
|----------|-----------|-------------|
| `tcp_listen` | `tcp_listen(host, port) -> Int` | Create a listening socket, returns fd |
| `tcp_accept` | `tcp_accept(fd) -> Int` | Accept a client connection, returns fd |
| `tcp_connect` | `tcp_connect(host, port) -> Int` | Connect to a server, returns fd |
| `tcp_read` | `tcp_read(fd) -> String` | Read until EOF |
| `tcp_read_bytes` | `tcp_read_bytes(fd, n) -> String` | Read exactly n bytes |
| `tcp_write` | `tcp_write(fd, data) -> Bool` | Write data to socket |
| `tcp_close` | `tcp_close(fd)` | Close a socket |
| `tcp_peer_addr` | `tcp_peer_addr(fd) -> String` | Get peer address as `"host:port"` |
| `tcp_set_timeout` | `tcp_set_timeout(fd, secs) -> Bool` | Set read/write timeout |

### Networking (TLS, requires OpenSSL)

| Function | Signature | Description |
|----------|-----------|-------------|
| `tls_connect` | `tls_connect(host, port) -> Int` | Create a TLS connection, returns fd |
| `tls_read` | `tls_read(fd) -> String` | Read decrypted data until EOF |
| `tls_read_bytes` | `tls_read_bytes(fd, n) -> String` | Read exactly n decrypted bytes |
| `tls_write` | `tls_write(fd, data) -> Bool` | Write encrypted data |
| `tls_close` | `tls_close(fd)` | Close a TLS connection |
| `tls_available` | `tls_available() -> Bool` | Check if TLS support is compiled in |

### Metaprogramming

| Function | Signature | Description |
|----------|-----------|-------------|
| `lat_eval` | `lat_eval(src) -> Value` | Run a string as Lattice code |
| `tokenize` | `tokenize(src) -> Array` | Tokenize source code into token strings |
| `is_complete` | `is_complete(src) -> Bool` | Check if source code has balanced delimiters |
| `require` | `require(path) -> Bool` | Load and execute a `.lat` file |

### String Methods

| Method | Description |
|--------|-------------|
| `.len()` | Length of the string |
| `.contains(substr)` | Check if string contains a substring |
| `.starts_with(prefix)` | Check if string starts with prefix |
| `.ends_with(suffix)` | Check if string ends with suffix |
| `.trim()` | Remove leading and trailing whitespace |
| `.to_upper()` | Convert to uppercase |
| `.to_lower()` | Convert to lowercase |
| `.replace(old, new)` | Replace all occurrences of `old` with `new` |
| `.split(delim)` | Split string by delimiter, returns array |
| `.index_of(substr)` | Index of first occurrence (-1 if not found) |
| `.substring(start, end)` | Extract substring by index range |
| `.chars()` | Split into array of individual characters |
| `.reverse()` | Reverse the string |
| `.repeat(n)` | Repeat the string `n` times |

### Array Methods

| Method | Description |
|--------|-------------|
| `.len()` | Number of elements |
| `.push(val)` | Append an element (mutates in place) |
| `.map(fn)` | Transform each element, returns new array |
| `.filter(fn)` | Keep elements where `fn` returns true |
| `.reduce(fn, init)` | Fold with `fn(accumulator, element)` |
| `.for_each(fn)` | Call `fn` on each element |
| `.find(fn)` | Return first element where `fn` returns true (or Unit) |
| `.join(sep)` | Join elements into a string with separator |
| `.contains(val)` | Check if array contains a value |
| `.reverse()` | Return a reversed copy |
| `.sort()` | Return a sorted copy (Int, Float, or String arrays) |
| `.slice(start, end)` | Extract subarray by index range |
| `.flat()` | Flatten one level of nested arrays |
| `.enumerate()` | Return array of `[index, value]` pairs |

### Map Methods

| Method | Description |
|--------|-------------|
| `.len()` | Number of entries |
| `.get(key)` | Get value by key (Unit if not found) |
| `.set(key, val)` | Set a key-value pair (mutates in place) |
| `.has(key)` | Check if key exists |
| `.remove(key)` | Remove a key-value pair |
| `.keys()` | Return array of all keys |
| `.values()` | Return array of all values |

### Channel Methods

| Method | Description |
|--------|-------------|
| `.send(val)` | Send a crystal value on the channel |
| `.recv()` | Receive a value (blocks until available, Unit if closed) |
| `.close()` | Close the channel |

### Operators

| Category | Operators |
|----------|-----------|
| Arithmetic | `+` `-` `*` `/` `%` |
| Comparison | `==` `!=` `<` `>` `<=` `>=` |
| Logical | `&&` `\|\|` `!` |
| Compound Assignment | `+=` `-=` `*=` `/=` `%=` |
| Range | `..` |
| Indexing | `[]` (with slice support) |
| String Concat | `+` |

## Examples

The `examples/` directory contains programs that demonstrate different aspects of the language:

| File | Description | Features |
|------|-------------|----------|
| `fibonacci.lat` | Iterative and recursive Fibonacci, golden ratio | Arrays, loops, recursion, float math |
| `primes.lat` | Sieve of Eratosthenes, factorization, Goldbach | Nested loops, math, formatted output |
| `sorting.lat` | Bubble, selection, and insertion sort | Array manipulation, closures, clone |
| `matrix.lat` | Matrix operations (add, multiply, transpose) | Nested arrays, function composition |
| `phase_demo.lat` | Phase system walkthrough | flux/fix, freeze/thaw, forge blocks, strict mode |
| `todo.lat` | Persistent todo list manager | File I/O, string parsing, data management |
| `ecs.lat` | Entity Component System game architecture | Structs, arrays, complex data flow |
| `state_machine.lat` | Vending machine simulator | Structs with callable fields, maps, dispatch |
| `string_tools.lat` | ROT13, Caesar cipher, word frequency | String methods, character operations, maps |

Run any example:

```sh
./clat examples/fibonacci.lat
```

## Self-Hosted REPL

Lattice includes a REPL written in Lattice itself (`repl.lat`), built on `is_complete`, `lat_eval`, and `input`:

```sh
./clat repl.lat
```

Supports multi-line input (auto-detects incomplete expressions), error handling via try/catch, and readline history.

## CLI Reference

```
clat [--stats] [--gc-stress] [file.lat]
```

| Flag | Description |
|------|-------------|
| `file.lat` | Run a Lattice source file |
| *(no file)* | Start the interactive REPL |
| `--stats` | Print memory/GC statistics to stderr after execution |
| `--gc-stress` | Force garbage collection on every allocation (for testing) |

## Building & Testing

```sh
make          # build the clat binary
make test     # run the test suite
make asan     # build and test with AddressSanitizer + UBSan
make clean    # remove build artifacts
```

**Required:** libedit — ships with macOS. On Linux, install `libedit-dev` (Debian/Ubuntu) or `libedit-devel` (Fedora/RHEL).

**Optional:** OpenSSL — enables TLS networking (`tls_*`) and crypto builtins (`sha256`, `md5`, `base64_*`). Detected automatically via pkg-config. Build without it using `make TLS=0`.

## License

BSD 3-Clause License. Copyright (c) 2026, Alex Jokela.

See [LICENSE](LICENSE) for the full text.
