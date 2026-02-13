# Lattice

A crystallization-based programming language implemented in C, where data transitions between fluid (mutable) and crystal (immutable) states through a unique phase system.

## Overview

Lattice is an interpreted programming language built around the metaphor of crystallization. Values begin in a **fluid** state where they can be freely modified, then **freeze** into an immutable **crystal** state for safe sharing and long-term storage. This phase system gives you explicit, fine-grained control over mutability — rather than relying on convention, the language enforces it.

The language features a familiar C-like syntax with modern conveniences: first-class closures, structs with callable fields, expression-based control flow, pattern matching, destructuring assignments, enums, sets, tuples, default parameters, variadic functions, string interpolation, nil coalescing, bitwise operators, import/module system, native extensions via `require_ext()`, try/catch error handling, structured concurrency with channels, and an interactive REPL with auto-display.

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
| `Enum` | Sum type with variants (`Color::Red`) |
| `Set` | Unordered collection of unique values (`Set::from([1, 2, 3])`) |
| `Tuple` | Fixed-size immutable sequence (`(1, "hello", true)`) |
| `Nil` | Explicit null value (`nil`) |
| `Unit` | Absence of a meaningful value (result of statements) |

### Functions

```lattice
fn add(a: Int, b: Int) -> Int {
    return a + b
}

fn greet(name: String) -> String {
    "Hello, ${name}!"  // string interpolation, implicit return
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

### Default Parameters & Variadic Functions

Functions and closures support default parameter values and variadic arguments:

```lattice
fn greet(name: String, greeting = "Hello") {
    print("${greeting}, ${name}!")
}
greet("Alice")            // Hello, Alice!
greet("Bob", "Hey")       // Hey, Bob!

fn sum(...nums) {
    nums.reduce(|a, b| a + b, 0)
}
print(sum(1, 2, 3))      // 6
```

### Pattern Matching

`match` expressions compare a value against patterns:

```lattice
let result = match value {
    0 => "zero",
    1..10 => "small",
    x if x > 100 => "big: ${x}",
    _ => "other"
}
```

Patterns support: integer/string/bool literals, ranges (`1..10`), variable bindings, guards (`if condition`), wildcards (`_`), and negative literals.

### Destructuring

Destructure arrays and structs/maps in `let` and `flux` bindings:

```lattice
let [a, b, c] = [1, 2, 3]
let [head, ...rest] = [10, 20, 30, 40]    // head = 10, rest = [20, 30, 40]
let {name, age} = person                   // extract fields by name
```

### Enums

Enums define sum types with unit or tuple variants:

```lattice
enum Color {
    Red,
    Green,
    Blue,
    Custom(Int, Int, Int)
}

let c = Color::Red
let rgb = Color::Custom(255, 128, 0)
print(rgb.payload())     // [255, 128, 0]
print(c.variant_name())  // Red
print(c.is_variant("Red")) // true
```

### Sets

Sets are unordered collections of unique values:

```lattice
let s = Set::from([1, 2, 3, 2, 1])  // {1, 2, 3}
s.add(4)
print(s.has(2))    // true
print(s.len())     // 4

let a = Set::from([1, 2, 3])
let b = Set::from([2, 3, 4])
print(a.union(b))        // {1, 2, 3, 4}
print(a.intersection(b)) // {2, 3}
print(a.difference(b))   // {1}
```

### Tuples

Tuples are fixed-size, immutable sequences of values:

```lattice
let point = (10, 20)
let record = ("Alice", 30, true)
print(point.0)          // 10
print(record.1)         // 30
print(record.len())     // 3
print(typeof(point))    // Tuple
```

### Nil & Nil Coalescing

`nil` represents an explicit null value, distinct from `unit`:

```lattice
let x = nil
print(typeof(x))        // Nil

let name = nil ?? "default"   // "default"
let val = "hello" ?? "other"  // "hello"

// nil coalescing chains
let result = nil ?? nil ?? "found"  // "found"
```

Map `.get()` returns `nil` for missing keys, making `??` useful for defaults:

```lattice
let m = Map::new()
let port = m.get("port") ?? 8080
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

Structs with a `repr` closure field use it for custom display:

```lattice
struct Point {
    x: Int,
    y: Int,
    repr: Fn
}

let p = Point {
    x: 10, y: 20,
    repr: |self| "(${self.x}, ${self.y})"
}
print(repr(p))  // (10, 20)
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

### Single-Quote Strings

Single-quoted strings have no interpolation — useful for patterns, regex, and templates:

```lattice
let pattern = 'hello ${world}'   // literal ${world}, no interpolation
let path = 'C:\Users\name'       // backslash-friendly
```

### Multi-Line Strings

Triple-quoted strings (`"""..."""`) preserve newlines and auto-dedent based on closing indent:

```lattice
let html = """
    <div>
        <p>Hello</p>
    </div>
    """
// Leading whitespace is stripped based on closing """ indent
```

Triple-quoted strings support interpolation:

```lattice
let name = "Lattice"
let msg = """
    Hello, ${name}!
    Version ${version()}
    """
```

### Spread Operator

The `...` spread operator expands arrays inline within array literals:

```lattice
let a = [1, 2, 3]
let b = [0, ...a, 4]     // [0, 1, 2, 3, 4]

let x = [10, 20]
let y = [30, 40]
let z = [...x, ...y]     // [10, 20, 30, 40]
```

### Bitwise Operators

Full set of bitwise operations on integers:

```lattice
print(0xFF & 0x0F)   // 15 (AND)
print(0xF0 | 0x0F)   // 255 (OR)
print(0xFF ^ 0x0F)    // 240 (XOR)
print(~0)             // -1 (NOT)
print(1 << 8)         // 256 (left shift)
print(256 >> 4)       // 16 (right shift)

// Compound assignment
flux x = 0xFF
x &= 0x0F             // x = 15
x |= 0xF0             // x = 255
x ^= 0x0F             // x = 240
x <<= 4               // x = 3840
x >>= 8               // x = 15
```

### Import

Import functions and values from other `.lat` files:

```lattice
import "math_utils.lat"              // import all exports
import "utils.lat" as utils          // namespaced import
import { add, subtract } from "math.lat"  // selective import
```

Imported modules export values by returning a Map:

```lattice
// math_utils.lat
fn add(a, b) { a + b }
fn sub(a, b) { a - b }
let exports = Map::new()
exports.set("add", add)
exports.set("sub", sub)
exports  // return the exports map
```

### Native Extensions

Lattice supports native extensions — shared libraries (`.dylib` on macOS, `.so` on Linux) loaded at runtime via `require_ext()`. Extensions register functions through a C API and appear as a Map of callable closures in Lattice code.

```lattice
let pg = require_ext("pg")

let conn = pg.get("connect")("host=localhost dbname=mydb")
let rows = pg.get("query")(conn, "SELECT id, name FROM users")

for row in rows {
    print("${row.get("id")}: ${row.get("name")}")
}

pg.get("close")(conn)
```

**Extension search paths** (checked in order):
1. `./extensions/<name>.dylib` (or `.so`)
2. `~/.lattice/ext/<name>.dylib`
3. `$LATTICE_EXT_PATH/<name>.dylib`

**PostgreSQL extension** — the first bundled extension, providing:

| Function | Description |
|----------|-------------|
| `pg.connect(connstr)` | Connect to PostgreSQL, returns a connection handle |
| `pg.query(conn, sql)` | Execute a query, returns array of Maps (one per row) |
| `pg.exec(conn, sql)` | Execute a statement (INSERT/UPDATE/DELETE), returns affected row count |
| `pg.status(conn)` | Connection status string (`"ok"` or `"bad"`) |
| `pg.close(conn)` | Close the connection |

Build the PostgreSQL extension with `make ext-pg` (requires libpq).

**Writing custom extensions:** compile a shared library against `lattice_ext.h` that exports a `lat_ext_init(LatExtContext *ctx)` function. Use `lat_ext_register()` to add functions, and the `lat_ext_*` helpers to construct and inspect values.

### Strict Mode

Enable `#mode strict` at the top of a file for stricter phase enforcement. In strict mode, `freeze()` on an identifier **consumes** the binding — the original variable is no longer accessible after freezing:

```lattice
#mode strict

flux data = [1, 2, 3]
fix frozen = freeze(data)
// data is no longer accessible here
```

## Standard Library

Lattice ships with 120+ builtin functions and 70+ type methods covering I/O, math, strings, files, networking, data formats, concurrency, and more.

### Core

| Function | Description |
|----------|-------------|
| `print(args...)` | Print values to stdout with newline |
| `eprint(args...)` | Print to stderr with newline |
| `print_raw(args...)` | Print to stdout without newline |
| `input(prompt?)` | Read a line from stdin (Unit on EOF) |
| `typeof(val)` | Type name of a value (`"Int"`, `"String"`, etc.) |
| `phase_of(val)` | Phase of a value (`"fluid"`, `"crystal"`, `"unphased"`) |
| `to_string(val)` | Convert any value to its string representation |
| `repr(val)` | Display representation (strings quoted, structs use custom `repr` if defined) |
| `len(val)` | Length of a string, array, or map |
| `assert(cond, msg?)` | Assert condition is truthy; error with optional message |
| `exit(code?)` | Exit the process (default code 0) |
| `version()` | Lattice version string |

### Phase Transitions

| Function | Description |
|----------|-------------|
| `freeze(val)` | Transition a value to crystal (immutable) phase |
| `thaw(val)` | Create a mutable copy of a crystal value |
| `clone(val)` | Deep-clone a value |

### Type Constructors & Conversion

| Function | Description |
|----------|-------------|
| `Map::new()` | Create an empty map |
| `Set::new()` | Create an empty set |
| `Set::from(array)` | Create a set from an array |
| `Channel::new()` | Create a channel for inter-thread communication |
| `range(start, end, step?)` | Generate array of integers (`start` inclusive, `end` exclusive) |
| `to_int(val)` | Convert to Int (from Float, Bool, or String) |
| `to_float(val)` | Convert to Float |
| `parse_int(s)` | Parse string as integer |
| `parse_float(s)` | Parse string as float |
| `ord(ch)` | Unicode code point of first character |
| `chr(code)` | Character from a code point |

### Error Handling

| Function | Description |
|----------|-------------|
| `error(msg)` | Create an error value from a message string |
| `is_error(val)` | Check if a value is an error |

### Math

| Function | Description |
|----------|-------------|
| `abs(n)` | Absolute value (preserves Int/Float type) |
| `floor(n)` | Floor to integer |
| `ceil(n)` | Ceiling to integer |
| `round(n)` | Round to nearest integer |
| `sqrt(n)` | Square root |
| `pow(base, exp)` | Exponentiation |
| `exp(x)` | e^x |
| `log(n)` | Natural logarithm |
| `log2(n)` | Base-2 logarithm |
| `log10(n)` | Base-10 logarithm |
| `min(a, b)` | Minimum of two numbers |
| `max(a, b)` | Maximum of two numbers |
| `clamp(x, lo, hi)` | Clamp value to range [lo, hi] |
| `sign(x)` | Returns -1, 0, or 1 |
| `lerp(a, b, t)` | Linear interpolation: `a + (b - a) * t` |
| `gcd(a, b)` | Greatest common divisor |
| `lcm(a, b)` | Least common multiple |
| `math_pi()` | Pi constant (3.14159...) |
| `math_e()` | Euler's number (2.71828...) |
| `random()` | Random float in [0, 1) |
| `random_int(lo, hi)` | Random integer in [lo, hi] inclusive |

**Trigonometric:**

| Function | Description |
|----------|-------------|
| `sin(x)` `cos(x)` `tan(x)` | Standard trig (radians) |
| `asin(x)` `acos(x)` `atan(x)` | Inverse trig |
| `atan2(y, x)` | Two-argument arc tangent |
| `sinh(x)` `cosh(x)` `tanh(x)` | Hyperbolic trig |

**Float inspection:**

| Function | Description |
|----------|-------------|
| `is_nan(x)` | True if value is NaN |
| `is_inf(x)` | True if value is infinite |

### String Formatting & Regex

| Function | Description |
|----------|-------------|
| `format(fmt, args...)` | Format string with `{}` placeholders |
| `regex_match(pattern, str)` | Test if POSIX extended regex matches |
| `regex_find_all(pattern, str)` | Find all non-overlapping matches |
| `regex_replace(pattern, str, repl)` | Replace all matches |

### JSON

| Function | Description |
|----------|-------------|
| `json_parse(str)` | Parse JSON string into a Lattice value |
| `json_stringify(val)` | Serialize a Lattice value to JSON |

### CSV

| Function | Description |
|----------|-------------|
| `csv_parse(str)` | Parse CSV into array of arrays (supports quoted fields) |
| `csv_stringify(data)` | Convert array of arrays to CSV string |

### TOML

| Function | Description |
|----------|-------------|
| `toml_parse(str)` | Parse TOML string into a Lattice Map |
| `toml_stringify(val)` | Serialize a Map to TOML string |

### YAML

| Function | Description |
|----------|-------------|
| `yaml_parse(str)` | Parse YAML string into a Lattice value |
| `yaml_stringify(val)` | Serialize a Map or Array to YAML string |

### HTTP Client

| Function | Description |
|----------|-------------|
| `http_get(url)` | HTTP GET request, returns Map with `status`, `headers`, `body` |
| `http_post(url, body)` | HTTP POST request with string body |
| `http_request(method, url, headers, body)` | Full HTTP request with custom method, headers map, and body |

### URL Encoding

| Function | Description |
|----------|-------------|
| `url_encode(str)` | Percent-encode a string for URLs |
| `url_decode(str)` | Decode a percent-encoded string |

### File System

| Function | Description |
|----------|-------------|
| `read_file(path)` | Read entire file contents as string |
| `write_file(path, content)` | Write string to file |
| `append_file(path, content)` | Append string to file |
| `file_exists(path)` | Check if file exists |
| `file_size(path)` | File size in bytes |
| `delete_file(path)` | Delete a file |
| `list_dir(path)` | List directory contents as array |
| `mkdir(path)` | Create a directory |
| `rmdir(path)` | Remove an empty directory |
| `rename(old, new)` | Rename/move a file or directory |
| `copy_file(src, dst)` | Copy a file |
| `chmod(path, mode)` | Change file permissions |
| `is_dir(path)` | Check if path is a directory |
| `is_file(path)` | Check if path is a regular file |
| `stat(path)` | File metadata (returns Map with `size`, `mtime`, `type`, `permissions`) |
| `glob(pattern)` | Match files by glob pattern (e.g., `"*.lat"`) |
| `realpath(path)` | Resolve to absolute canonical path |
| `tempdir()` | Create a temporary directory, returns path |
| `tempfile()` | Create a temporary file, returns path |

### Path Utilities

| Function | Description |
|----------|-------------|
| `path_join(parts...)` | Join path components with `/` |
| `path_dir(path)` | Directory portion of a path |
| `path_base(path)` | Base name of a path |
| `path_ext(path)` | File extension (including `.`) |

### Environment & Process

| Function | Description |
|----------|-------------|
| `env(name)` | Get environment variable (Unit if not set) |
| `env_set(name, value)` | Set environment variable |
| `env_keys()` | Array of all environment variable names |
| `cwd()` | Current working directory |
| `args()` | Command-line arguments as array |
| `exec(cmd)` | Run shell command, return stdout (error on non-zero exit) |
| `shell(cmd)` | Run shell command, return Map with `stdout`, `stderr`, `exit_code` |
| `platform()` | OS name (`"macos"`, `"linux"`, `"windows"`, `"wasm"`) |
| `hostname()` | System hostname |
| `pid()` | Current process ID |

### Date & Time

| Function | Description |
|----------|-------------|
| `time()` | Current time in milliseconds since epoch |
| `sleep(ms)` | Sleep for milliseconds |
| `time_format(epoch_ms, fmt)` | Format timestamp with strftime codes |
| `time_parse(str, fmt)` | Parse datetime string to epoch milliseconds |

### Cryptography (requires OpenSSL)

| Function | Description |
|----------|-------------|
| `sha256(s)` | SHA-256 hash as hex string |
| `md5(s)` | MD5 hash as hex string |
| `base64_encode(s)` | Base64 encode |
| `base64_decode(s)` | Base64 decode |

### Networking (TCP)

| Function | Description |
|----------|-------------|
| `tcp_listen(host, port)` | Create listening socket, returns fd |
| `tcp_accept(fd)` | Accept client connection, returns fd |
| `tcp_connect(host, port)` | Connect to server, returns fd |
| `tcp_read(fd)` | Read until EOF |
| `tcp_read_bytes(fd, n)` | Read exactly n bytes |
| `tcp_write(fd, data)` | Write data to socket |
| `tcp_close(fd)` | Close socket |
| `tcp_peer_addr(fd)` | Peer address as `"host:port"` |
| `tcp_set_timeout(fd, secs)` | Set read/write timeout |

### Networking (TLS, requires OpenSSL)

| Function | Description |
|----------|-------------|
| `tls_connect(host, port)` | Create TLS connection, returns fd |
| `tls_read(fd)` | Read decrypted data until EOF |
| `tls_read_bytes(fd, n)` | Read exactly n decrypted bytes |
| `tls_write(fd, data)` | Write encrypted data |
| `tls_close(fd)` | Close TLS connection |
| `tls_available()` | Check if TLS support is compiled in |

### Functional

| Function | Description |
|----------|-------------|
| `identity(x)` | Return x unchanged |
| `pipe(val, ...fns)` | Thread value through functions: `pipe(x, f, g)` = `g(f(x))` |
| `compose(f, g)` | Return new function: `compose(f, g)(x)` = `f(g(x))` |

### Metaprogramming

| Function | Description |
|----------|-------------|
| `lat_eval(src)` | Run a string as Lattice code |
| `tokenize(src)` | Tokenize source code into token strings |
| `is_complete(src)` | Check if source has balanced delimiters |
| `require(path)` | Load and execute a `.lat` file |
| `require_ext(name)` | Load a native extension, returns Map of functions |

### String Interpolation

Embed expressions directly inside string literals with `${...}`:

```lattice
let name = "world"
print("hello ${name}")              // hello world
print("${name} is ${30} years old") // world is 30 years old
print("2 + 2 = ${2 + 2}")           // 2 + 2 = 4
print("upper: ${name.to_upper()}")  // upper: WORLD
```

Use `\${` to produce a literal `${`:

```lattice
print("escaped: \${not interpolated}") // escaped: ${not interpolated}
```

### String Methods

| Method | Description |
|--------|-------------|
| `.len()` | Length of the string |
| `.contains(substr)` | Check if string contains substring |
| `.starts_with(prefix)` | Check if string starts with prefix |
| `.ends_with(suffix)` | Check if string ends with suffix |
| `.index_of(substr)` | Index of first occurrence (-1 if not found) |
| `.count(substr)` | Count non-overlapping occurrences |
| `.is_empty()` | True if string length is 0 |
| `.trim()` | Remove leading and trailing whitespace |
| `.trim_start()` | Remove leading whitespace |
| `.trim_end()` | Remove trailing whitespace |
| `.to_upper()` | Convert to uppercase |
| `.to_lower()` | Convert to lowercase |
| `.replace(old, new)` | Replace all occurrences |
| `.split(delim)` | Split by delimiter, returns array |
| `.substring(start, end)` | Extract substring by index range |
| `.chars()` | Split into array of characters |
| `.bytes()` | Array of byte values (integers) |
| `.reverse()` | Reverse the string |
| `.repeat(n)` | Repeat n times |
| `.pad_left(width, ch)` | Pad on the left to target width |
| `.pad_right(width, ch)` | Pad on the right to target width |

### Array Methods

| Method | Description |
|--------|-------------|
| `.len()` | Number of elements |
| `.push(val)` | Append element (mutates in place) |
| `.pop()` | Remove and return last element (mutates in place) |
| `.insert(i, val)` | Insert element at index (mutates in place) |
| `.remove_at(i)` | Remove element at index (mutates in place) |
| `.first()` | First element (Unit if empty) |
| `.last()` | Last element (Unit if empty) |
| `.index_of(val)` | Index of first occurrence (-1 if not found) |
| `.contains(val)` | Check if array contains value |
| `.map(fn)` | Transform each element |
| `.flat_map(fn)` | Map then flatten one level |
| `.filter(fn)` | Keep elements where fn returns true |
| `.reduce(fn, init)` | Fold with `fn(acc, elem)` |
| `.for_each(fn)` | Call fn on each element |
| `.find(fn)` | First element where fn is true (Unit if none) |
| `.any(fn)` | True if any element satisfies fn |
| `.all(fn)` | True if all elements satisfy fn |
| `.join(sep)` | Join into string with separator |
| `.reverse()` | Return reversed copy |
| `.sort()` | Return sorted copy |
| `.sort_by(fn)` | Sort by comparator: `fn(a, b)` returns negative/zero/positive |
| `.slice(start, end)` | Extract subarray by index range |
| `.take(n)` | First n elements |
| `.drop(n)` | All elements after first n |
| `.flat()` | Flatten one level of nesting |
| `.zip(other)` | Zip with another array into `[[a, b], ...]` pairs |
| `.unique()` | Remove duplicate values |
| `.chunk(n)` | Split into sub-arrays of size n |
| `.group_by(fn)` | Group by fn result into a Map |
| `.sum()` | Sum of numeric elements |
| `.min()` | Minimum numeric element |
| `.max()` | Maximum numeric element |
| `.enumerate()` | Array of `[index, value]` pairs |

### Map Methods

| Method | Description |
|--------|-------------|
| `.len()` | Number of entries |
| `.get(key)` | Get value by key (nil if not found) |
| `.set(key, val)` | Set key-value pair (mutates in place) |
| `.has(key)` | Check if key exists |
| `.remove(key)` | Remove a key-value pair |
| `.keys()` | Array of all keys |
| `.values()` | Array of all values |
| `.entries()` | Array of `[key, value]` pairs |
| `.merge(other)` | Merge another map in (mutates in place) |
| `.for_each(fn)` | Call `fn(key, value)` for each entry |
| `.filter(fn)` | New map with entries where `fn(key, value)` is true |
| `.map(fn)` | New map with values transformed by `fn(key, value)` |

### Channel Methods

| Method | Description |
|--------|-------------|
| `.send(val)` | Send a crystal value on the channel |
| `.recv()` | Receive a value (blocks until available, Unit if closed) |
| `.close()` | Close the channel |

### Enum Methods

| Method | Description |
|--------|-------------|
| `.variant_name()` | Name of the variant as a string |
| `.enum_name()` | Name of the enum type |
| `.is_variant(name)` | Check if enum is a specific variant |
| `.payload()` | Extract tuple variant data as array (Unit for unit variants) |

### Set Methods

| Method | Description |
|--------|-------------|
| `.len()` | Number of elements |
| `.add(val)` | Add an element |
| `.has(val)` | Check if element exists |
| `.remove(val)` | Remove an element |
| `.to_array()` | Convert to array |
| `.union(other)` | Set union |
| `.intersection(other)` | Set intersection |
| `.difference(other)` | Set difference |
| `.is_subset(other)` | Check if subset |
| `.is_superset(other)` | Check if superset |

### Tuple Access

| Method | Description |
|--------|-------------|
| `.0`, `.1`, `.2`, ... | Access element by index |
| `.len()` | Number of elements |

### Operators

| Category | Operators |
|----------|-----------|
| Arithmetic | `+` `-` `*` `/` `%` |
| Comparison | `==` `!=` `<` `>` `<=` `>=` |
| Logical | `&&` `\|\|` `!` |
| Bitwise | `&` `\|` `^` `~` `<<` `>>` |
| Compound Assignment | `+=` `-=` `*=` `/=` `%=` `&=` `\|=` `^=` `<<=` `>>=` |
| Nil Coalescing | `??` |
| Spread | `...expr` (in array literals) |
| Range | `..` |
| Indexing | `[]` (with slice support) |
| String Concat | `+` |
| String Interpolation | `"hello ${expr}"` |

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

## Interactive REPL

The built-in REPL automatically displays expression results with a `=> ` prefix:

```
lattice> 42
=> 42
lattice> "hello"
=> "hello"
lattice> [1, 2, 3].map(|x| x * 2)
=> [2, 4, 6]
lattice> let x = 5
lattice>
```

Assignments and statements that return Unit are silently suppressed. The REPL supports multi-line input (auto-detects incomplete expressions) and readline history.

Lattice also includes a self-hosted REPL written in Lattice itself (`repl.lat`), built on `is_complete`, `lat_eval`, and `input`:

```sh
./clat repl.lat
```

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
make ext-pg   # build the PostgreSQL extension (requires libpq)
make clean    # remove build artifacts
```

**Required:** libedit — ships with macOS. On Linux, install `libedit-dev` (Debian/Ubuntu) or `libedit-devel` (Fedora/RHEL).

**Optional:** OpenSSL — enables TLS networking (`tls_*`) and crypto builtins (`sha256`, `md5`, `base64_*`). Detected automatically via pkg-config. Build without it using `make TLS=0`.

## License

BSD 3-Clause License. Copyright (c) 2026, Alex Jokela.

See [LICENSE](LICENSE) for the full text.
