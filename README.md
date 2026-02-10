# Lattice

A crystallization-based programming language implemented in C, where data transitions between fluid (mutable) and crystal (immutable) states through a unique phase system.

## Overview

Lattice is an interpreted programming language built around the metaphor of crystallization. Values begin in a **fluid** state where they can be freely modified, then **freeze** into an immutable **crystal** state for safe sharing and long-term storage. This phase system gives you explicit, fine-grained control over mutability — rather than relying on convention, the language enforces it.

The language features a familiar C-like syntax with modern conveniences: first-class closures, structs with callable fields, expression-based control flow, pattern-style `for..in` iteration, try/catch error handling, and a self-hosted REPL written in Lattice itself. File I/O, string manipulation, and metaprogramming via `lat_eval` round out the standard library.

Lattice compiles and runs on macOS and Linux with no dependencies beyond a C11 compiler and libedit.

## Quick Start

**Prerequisites:** C compiler with C11 support, libedit (ships with macOS; `libedit-dev` on Debian/Ubuntu)

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
| `Struct` | Named record type with typed fields |
| `Closure` | First-class function value (`|x| x + 1`) |
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

### Strict Mode

Enable `#mode strict` at the top of a file for stricter phase enforcement. In strict mode, `freeze()` on an identifier **consumes** the binding — the original variable is no longer accessible after freezing:

```lattice
#mode strict

flux data = [1, 2, 3]
fix frozen = freeze(data)
// data is no longer accessible here
```

## Standard Library

### Builtin Functions

| Function | Signature | Description |
|----------|-----------|-------------|
| `print` | `print(args...)` | Print values to stdout with newline |
| `eprint` | `eprint(args...)` | Print values to stderr with newline |
| `print_raw` | `print_raw(args...)` | Print values to stdout without newline |
| `input` | `input(prompt?) -> String\|Unit` | Read a line from stdin (returns Unit on EOF) |
| `typeof` | `typeof(val) -> String` | Returns the type name of a value |
| `phase_of` | `phase_of(val) -> String` | Returns `"fluid"`, `"crystal"`, or `"unphased"` |
| `to_string` | `to_string(val) -> String` | Convert any value to its string representation |
| `len` | `len(val) -> Int` | Length of a string or array |
| `parse_int` | `parse_int(s) -> Int` | Parse a string as an integer |
| `parse_float` | `parse_float(s) -> Float` | Parse a string as a float |
| `ord` | `ord(ch) -> Int` | ASCII code of the first character |
| `chr` | `chr(code) -> String` | Character from an ASCII code |
| `read_file` | `read_file(path) -> String` | Read entire file contents |
| `write_file` | `write_file(path, content) -> Bool` | Write string to a file |
| `exit` | `exit(code)` | Exit the process with a status code |
| `version` | `version() -> String` | Returns the Lattice version string |
| `is_complete` | `is_complete(src) -> Bool` | Check if source code has balanced delimiters |
| `lat_eval` | `lat_eval(src) -> Value` | Evaluate a string as Lattice code |
| `tokenize` | `tokenize(src) -> Array` | Tokenize source code into an array of token strings |
| `Map::new` | `Map::new() -> Map` | Create an empty map |
| `freeze` | `freeze(val) -> crystal` | Transition a value to crystal (immutable) phase |
| `thaw` | `thaw(val) -> fluid` | Create a mutable copy of a crystal value |
| `clone` | `clone(val) -> Value` | Deep-clone a value |

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
| `.for_each(fn)` | Call `fn` on each element |
| `.find(fn)` | Return first element where `fn` returns true |
| `.join(sep)` | Join elements into a string with separator |
| `.contains(val)` | Check if array contains a value |
| `.reverse()` | Return a reversed copy |
| `.enumerate()` | Return array of `[index, value]` pairs |

### Map Methods

| Method | Description |
|--------|-------------|
| `.len()` | Number of entries |
| `.get(key)` | Get value by key |
| `.set(key, val)` | Set a key-value pair (mutates in place) |
| `.has(key)` | Check if key exists |
| `.remove(key)` | Remove a key-value pair |
| `.keys()` | Return array of all keys |
| `.values()` | Return array of all values |

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
make test     # run 175 unit tests
make asan     # build and test with AddressSanitizer + UBSan
make clean    # remove build artifacts
```

**Dependency:** libedit — ships with macOS. On Linux, install `libedit-dev` (Debian/Ubuntu) or `libedit-devel` (Fedora/RHEL).

## License

BSD 3-Clause License. Copyright (c) 2026, Alex Jokela.

See [LICENSE](LICENSE) for the full text.
