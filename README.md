# Lattice

A crystallization-based programming language implemented in C, where data transitions between fluid (mutable) and crystal (immutable) states through a unique phase system.

## Overview

Lattice is an interpreted programming language built around the metaphor of crystallization. Values begin in a **fluid** state where they can be freely modified, then **freeze** into an immutable **crystal** state for safe sharing and long-term storage. This phase system gives you explicit, fine-grained control over mutability — rather than relying on convention, the language enforces it.

The language features a familiar C-like syntax with modern conveniences: first-class closures, structs with callable fields, traits with `impl` blocks, expression-based control flow, pattern matching with exhaustiveness checking and array/struct destructuring, enums, sets, tuples, lazy iterators with chainable transforms, default parameters, variadic functions, string interpolation, nil coalescing, optional chaining (`?.`), bitwise operators, import/module system with namespace isolation, native extensions via `require_ext()`, try/catch error handling, `defer` for guaranteed cleanup, Result `?` operator for ergonomic error propagation, function contracts (`require`/`ensure`), runtime type checking, structured concurrency with channels and `select` multiplexing, phase constraints with phase-dependent dispatch, phase reactions, crystallize blocks, phase borrowing, sublimation (shallow freeze), freeze-except (defects), seed/grow contracts, phase pressure, bond strategies, alloy structs (per-field phase declarations), `@fluid`/`@crystal` annotations, composite phase constraints, bytecode compilation to `.latc` files (with both C and self-hosted compiler backends), opt-in mark-sweep garbage collector, built-in test framework with assertions, `clat fmt` source formatter, `clat doc` documentation generator, interactive debugger (`--debug`) and in-code `breakpoint()`, REPL with tab completion and auto-display, and a self-hosted package registry.

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
| `Int` | 64-bit signed integer (`42`, `0xFF`, `1_000_000`) |
| `Float` | 64-bit double-precision float (`3.14`, `1_000.5`) |
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
| `Iterator` | Lazy sequence with chainable transforms (`iter([1,2,3])`) |
| `Nil` | Explicit null value (`nil`) |
| `Unit` | Absence of a meaningful value (result of statements) |

### Number Literals

Integer and float literals support underscores between digits for readability. Integer literals may also be written in hexadecimal with a `0x` prefix:

```lattice
let million = 1_000_000       // underscore separators
let color = 0xFF              // hexadecimal (255)
let mask = 0xDEAD_BEEF        // hex with underscores
let pi = 3.14_159             // float with underscores
let plain = 42                // regular integer
```

### Functions

```lattice
fn add(a: Int, b: Int) -> Int {
    return a + b
}

fn greet(name: String) -> String {
    "Hello, ${name}!"  // string interpolation, implicit return
}
```

Type annotations are enforced at runtime. Passing a value of the wrong type produces a clear error:

```lattice
fn add(a: Int, b: Int) -> Int { return a + b }
add(1, "hello")  // error: function 'add' parameter 'b' expects type Int, got String
```

Supported type names: `Int`, `Float`, `String`, `Bool`, `Nil`, `Map`, `Array`, `Fn`, `Channel`, `Range`, `Set`, `Tuple`, `Number` (Int or Float), `Any` (accepts everything), struct names, and enum names. Array element types are checked with `[T]` syntax:

```lattice
fn sum(nums: [Int]) -> Int {
    flux total = 0
    for n in nums { total += n }
    return total
}
```

### Function Contracts

Functions can declare preconditions (`require`) and postconditions (`ensure`) between the parameter list and the body:

```lattice
fn withdraw(account: Map, amount: Int) -> Int
    require amount > 0, "amount must be positive"
    require account.get("balance") >= amount, "insufficient funds"
    ensure |result| { result >= 0 }, "balance must stay non-negative"
{
    return account.get("balance") - amount
}
```

`require` clauses are checked before the body executes. `ensure` clauses receive the return value and are checked after. Both are gated by the `--no-assertions` flag.

### Phase Constraints

Function parameters can declare a required phase using `flux`/`fix` (or `~`/`*`) in the type annotation. The runtime rejects arguments with an incompatible phase:

```lattice
fn mutate(data: flux Map) { data.set("key", "value") }
fn inspect(data: fix Map) { print(data.get("key")) }

flux m = Map::new()
mutate(m)       // ok — fluid argument matches flux constraint
inspect(m)      // error — fluid argument rejected by fix constraint

fix frozen = freeze(m)
inspect(frozen) // ok — crystal argument matches fix constraint
```

### Phase-Dependent Dispatch

Define multiple functions with the same name but different phase annotations. The runtime dispatches to the best-matching overload based on argument phases:

```lattice
fn process(data: ~Map) { print("mutable path") }
fn process(data: *Map) { print("immutable path") }

flux m = Map::new()
process(m)              // "mutable path"
freeze(m)
process(m)              // "immutable path"
```

### Composite Phase Constraints

Use union syntax `(~|*)` in parameter type annotations to accept multiple phases:

```lattice
fn process(data: (~|*) Map) {
    // accepts both fluid and crystal Maps
    print(phase_of(data))
}

flux m = Map::new()
process(m)             // "fluid"
fix f = freeze(m)
process(f)             // "crystal"
```

Phase symbols can be combined with `|`: `~` (fluid), `*` (crystal), `sublimated`. Equivalent keyword forms also work: `(flux|fix)`.

### Crystallization Contracts

Attach a validation closure to `freeze()` with `where`. The contract runs before the value is frozen and rejects invalid state:

```lattice
flux config = Map::new()
config["host"] = "localhost"
config["port"] = 8080

// Contract validates the value before freezing
freeze(config) where |v| {
    if !v.has("host") { throw("missing host") }
    if !v.has("port") { throw("missing port") }
}
// config is now crystal — validated and immutable
```

### Phase Propagation (Bonds)

Link variables so that freezing one cascades to its dependencies:

```lattice
flux header = Map::new()
flux body = Map::new()
flux footer = Map::new()

bond(header, body, footer)  // body and footer are bonded to header

freeze(header)              // freezes header, body, AND footer
print(phase_of(body))       // "crystal"
print(phase_of(footer))     // "crystal"
```

Use `unbond(target, dep)` to remove a bond before freezing.

### Phase Reactions

Register callbacks that fire automatically when a variable's phase changes:

```lattice
flux data = [1, 2, 3]
react(data, |phase, val| {
    print("data is now " + phase + ": " + to_string(val))
})
freeze(data)   // prints: "data is now crystal: [1, 2, 3]"
thaw(data)     // prints: "data is now fluid: [1, 2, 3]"
unreact(data)  // removes all reactions
```

### Phase History (Temporal Values)

Track a variable's phase transitions and value changes over time:

```lattice
flux counter = 0
track("counter")

counter = 10
counter = 20
freeze(counter)

let history = phases("counter")
// [{phase: "fluid", value: 0}, {phase: "fluid", value: 10},
//  {phase: "fluid", value: 20}, {phase: "crystal", value: 20}]

let old_val = rewind("counter", 2)  // 10 (two steps back)
```

### Crystallize Blocks

Temporarily freeze a variable for the duration of a block, then auto-restore its original phase:

```lattice
flux data = [1, 2, 3]
crystallize(data) {
    // data is crystal here — mutations rejected
    print(phase_of(data))  // "crystal"
}
// data is fluid again
data.push(4)  // works
```

### Phase Borrowing

Temporarily thaw a crystal variable for the duration of a block, then auto-restore its original phase. The inverse of `crystallize()`:

```lattice
let data = freeze([1, 2, 3])
borrow(data) {
    // data is fluid here — mutations allowed
    data.push(4)
    print(phase_of(data))  // "fluid"
}
// data is crystal again, with the mutation preserved
print(data.len())  // 4
```

If the variable is already fluid, `borrow` simply runs the body without changing anything.

### Sublimation (Shallow Freeze)

`sublimate()` locks a value's top-level structure while leaving inner values mutable — a "shallow freeze":

```lattice
flux items = [Map::new(), Map::new()]
items[0]["name"] = "A"
items[1]["name"] = "B"

sublimate(items)
// items.push(...)  — error: sublimated, structural changes not allowed
// items[0]["name"] = "C"  — allowed: inner values still mutable
print(phase_of(items))  // "sublimated"

thaw(items)  // restores to fluid
```

### Freeze Except (Defects)

Freeze a struct or map while exempting specific fields or keys from crystallization:

```lattice
struct User { name: String, age: Int, score: Int }
flux user = User { name: "Alice", age: 30, score: 0 }
freeze(user) except ["score"]
// user.name and user.age are crystal, user.score stays fluid
user.score = 100  // allowed
// user.name = "Bob"  — error: field is crystal
```

### Seed Crystals

Attach a pending contract to a fluid variable. When it's eventually frozen, the contract must pass. Use `grow()` to trigger freeze + contract validation:

```lattice
flux val = 50
seed(val, |v| { v > 0 && v < 100 })
val = 75
grow(val)  // freezes + validates the seed contract
// val is now crystal

unseed(var)  // remove a pending seed contract
```

### Phase Pressure

Restrict which structural operations are allowed on a fluid variable without fully freezing it:

```lattice
flux data = [1, 2, 3]
pressurize(data, "no_grow")   // allow mutation but not push/pop
data[0] = 10    // allowed
// data.push(4)  — error: pressurized — structural changes not allowed
depressurize(data)
data.push(4)    // works again
```

Pressure modes: `"no_grow"` (no push/insert), `"no_shrink"` (no pop/remove), `"no_resize"` (neither).

Use `pressure_of(var)` to query the current pressure mode (returns nil if none).

### Bond Strategies (Phase Interference)

Extend `bond()` with a strategy parameter controlling how phase changes propagate:

```lattice
flux a = [1, 2, 3]
flux b = [4, 5, 6]

// "mirror" (default) — when a freezes, b freezes too
bond(a, b, "mirror")

// "inverse" — when a freezes, b thaws (and vice versa)
bond(a, b, "inverse")

// "gate" — a can only freeze after b is crystal
bond(a, b, "gate")
```

### Alloys (Per-Field Phase Declarations)

Declare per-field phase constraints in struct definitions. Fields automatically get their declared phase on instantiation:

```lattice
struct Config {
    host: fix String,    // always crystal
    port: fix Int,       // always crystal
    retries: flux Int,   // always fluid
}

let cfg = Config { host: "localhost", port: 8080, retries: 0 }
cfg.retries = 5    // allowed (flux field)
// cfg.host = "other"  — error: field is crystal
```

### Phase Annotations

Use `@fluid` or `@crystal` before a declaration to assert its phase at compile time. No runtime effect — purely a static check:

```lattice
@crystal
fix config = freeze({ port: 8080, host: "localhost" })

@fluid
fn process(data: any) {
    data.push(1)
}
```

In strict mode, the phase checker validates that the initializer's phase matches the annotation. Mismatches produce a compile-time error.

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

Patterns support: integer/string/bool literals, ranges (`1..10`), variable bindings, guards (`if condition`), wildcards (`_`), negative literals, array destructuring (`[a, b, ...rest]`), and struct destructuring (`{field: pat}`). The compiler warns on non-exhaustive matches (missing enum variants, bool cases, or absent wildcard arms).

```lattice
match point {
    [0, 0] => "origin",
    [x, 0] => "on x-axis at ${x}",
    [_, ...rest] => "rest: ${rest}",
    _ => "other"
}
match user {
    {role: "admin"} => "admin user",
    {name, age} => "${name} is ${age}"
}
```

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

### Iterators

Lazy sequences with chainable transforms. Create from arrays, ranges, or repeat values:

```lattice
let it = iter([1, 2, 3, 4, 5])
let doubled = it.map(|x| x * 2).filter(|x| x > 4).collect()  // [6, 8, 10]

let r = range_iter(1, 10).take(3).collect()   // [1, 2, 3]
let inf = repeat_iter(42).take(5).collect()   // [42, 42, 42, 42, 42]

// Chainable: map, filter, take, skip, enumerate, zip
// Consumers: collect, reduce, any, all, count
for val in iter([10, 20, 30]) {
    print(val)
}
```

### Nil, Nil Coalescing & Optional Chaining

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

**Optional chaining** (`?.`, `?[`) safely navigates nullable values. If the receiver is nil, the entire chain short-circuits to nil instead of erroring:

```lattice
let user = nil
print(user?.name)           // nil (no error)
print(user?.address?.city)  // nil (chain short-circuits)
print(user?[0])             // nil (optional index)

// Only the marked access is optional:
// user?.name.len()  — if user is nil, ?.name returns nil, then .len() on nil errors

// Compose with ?? for defaults:
let city = user?.address?.city ?? "Unknown"
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

### Traits & Impl

Define shared behavior with traits and implement them for structs:

```lattice
trait Geometric {
    fn area(self: Any) -> Float
    fn scale(self: Any, factor: Float) -> Any
}

impl Geometric for Point {
    fn area(self: Any) -> Float {
        return to_float(self.x * self.y)
    }
    fn scale(self: Any, factor: Float) -> Any {
        return Point { x: self.x * factor, y: self.y * factor }
    }
}

let p = Point { x: 3, y: 4 }
print(p.area())       // 12.0
let q = p.scale(2.0)
print(q.x)            // 6.0
```

Trait methods are dispatched via the struct type — `p.area()` resolves to `Point::area(p)`.

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

### Defer

`defer` guarantees cleanup code runs when the enclosing scope exits, whether normally, via early return, or on error. Multiple defers execute in LIFO (last-in, first-out) order:

```lattice
fn process_file(path: String) {
    let fd = open(path)
    defer { close(fd) }           // guaranteed cleanup

    let lock = acquire_lock()
    defer { release_lock(lock) }  // runs before close(fd)

    // ... work with fd and lock ...
    // both defers fire on any exit path
}
```

Defers fire per block scope, making them useful in loops too:

```lattice
for file in files {
    defer { print("done with " + file) }
    process(file)
}
// "done with ..." prints after each iteration
```

### Result ? Operator

The `?` postfix operator provides ergonomic error propagation for Result maps (Maps with `"tag"` set to `"ok"` or `"err"` and a `"value"` field). If the result is `ok`, `?` unwraps the value. If `err`, it immediately returns the error from the enclosing function:

```lattice
import "lib/fn" as F

fn load_config(path: String) -> Map {
    let text = F::try_fn(|_| { read_file(path) })?
    let parsed = F::try_fn(|_| { json_parse(text) })?
    return F::ok(parsed)
}

fn main() {
    let result = load_config("config.json")
    if F::is_err(result) {
        print("Failed: " + F::unwrap_err(result))
    } else {
        print("Loaded: " + to_string(F::unwrap(result)))
    }
}
```

Works with the `ok()`/`err()` Result pattern from `lib/fn.lat` — no special type needed, just a Map with `{"tag": "ok", "value": ...}` or `{"tag": "err", "value": ...}`.

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

**`select`** waits on multiple channels and executes the arm for whichever receives a value first:

```lattice
let result = select {
    msg from ch1 => { handle(msg) }
    val from ch2 => { process(val) }
    timeout(1000) => { print("timed out") }
    default => { fallback() }
}
```

Select arms:
- **`binding from channel => { body }`** — receive from a channel and bind the value
- **`default => { body }`** — execute immediately if no channels are ready (non-blocking)
- **`timeout(ms) => { body }`** — execute if no value arrives within the timeout

Channel arms are checked in random order for fairness. If all channels are closed and there's no default arm, `select` returns Unit.

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

#### Built-in Modules

Lattice provides built-in standard library modules that resolve without file I/O. Use `import` with a bare module name (no path separators or `.lat` extension):

```lattice
import "math" as m            // full module as a Map
import { sin, cos } from "math"  // selective import
import { parse } from "json"
```

| Module | Contents |
|--------|----------|
| `math` | `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `atan2`, `sinh`, `cosh`, `tanh`, `sqrt`, `pow`, `abs`, `floor`, `ceil`, `round`, `log`, `log2`, `log10`, `exp`, `min`, `max`, `clamp`, `sign`, `gcd`, `lcm`, `lerp`, `is_nan`, `is_inf`, `random`, `random_int`, `PI`, `E` |
| `fs` | `read_file`, `write_file`, `read_file_bytes`, `write_file_bytes`, `append_file`, `copy_file`, `file_exists`, `is_file`, `is_dir`, `file_size`, `delete_file`, `mkdir`, `rmdir`, `rename`, `chmod`, `list_dir`, `glob`, `stat`, `realpath`, `tempdir`, `tempfile` |
| `path` | `join`, `dir`, `base`, `ext` |
| `json` | `parse`, `stringify` |
| `toml` | `parse`, `stringify` |
| `yaml` | `parse`, `stringify` |
| `crypto` | `sha256`, `md5`, `base64_encode`, `base64_decode`, `url_encode`, `url_decode` |
| `http` | `get`, `post`, `request` |
| `net` | `tcp_listen`, `tcp_accept`, `tcp_connect`, `tcp_read`, `tcp_read_bytes`, `tcp_write`, `tcp_close`, `tcp_peer_addr`, `tcp_set_timeout`, `tls_connect`, `tls_read`, `tls_read_bytes`, `tls_write`, `tls_close`, `tls_available` |
| `os` | `exec`, `shell`, `env`, `env_set`, `env_keys`, `cwd`, `platform`, `hostname`, `pid`, `args` |
| `time` | `now`, `sleep`, `format`, `parse` |
| `regex` | `match`, `find_all`, `replace` |

All functions remain available as flat globals (e.g. `sin(x)`, `read_file(path)`) for backwards compatibility.

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
2. `./extensions/<name>/<name>.dylib`
3. `~/.lattice/ext/<name>.dylib`
4. `$LATTICE_EXT_PATH/<name>.dylib`

**PostgreSQL extension** — requires libpq:

| Function | Description |
|----------|-------------|
| `pg.connect(connstr)` | Connect to PostgreSQL, returns a connection handle |
| `pg.query(conn, sql)` | Execute a query, returns array of Maps (one per row) |
| `pg.exec(conn, sql)` | Execute a statement (INSERT/UPDATE/DELETE), returns affected row count |
| `pg.status(conn)` | Connection status string (`"ok"` or `"bad"`) |
| `pg.close(conn)` | Close the connection |

Build the PostgreSQL extension with `make ext-pg` (requires libpq).

**SQLite extension** — requires libsqlite3 (ships with macOS; `libsqlite3-dev` on Debian/Ubuntu):

```lattice
let db = require_ext("sqlite")

let conn = db.get("open")("mydata.db")
db.get("exec")(conn, "CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT, age INTEGER)")
db.get("exec")(conn, "INSERT INTO users (name, age) VALUES ('Alice', 30)")

let rows = db.get("query")(conn, "SELECT * FROM users")
for row in rows {
    print(row)
}

db.get("close")(conn)
```

| Function | Description |
|----------|-------------|
| `sqlite.open(path)` | Open/create an SQLite database file, returns connection handle |
| `sqlite.query(conn, sql [, params])` | Execute a SELECT query, returns array of Maps. Optional `params` array for `?` placeholders. |
| `sqlite.exec(conn, sql [, params])` | Execute a statement, returns affected row count. Optional `params` array for `?` placeholders. |
| `sqlite.last_insert_rowid(conn)` | Returns the rowid of the most recent INSERT |
| `sqlite.status(conn)` | Connection status (`"ok"` or `"closed"`) |
| `sqlite.close(conn)` | Close the database connection |

**Parameterized queries** use `?` placeholders with an array of values to prevent SQL injection:

```lattice
let db = require_ext("sqlite")
let open_fn = db.get("open")
let run = db.get("exec")
let query = db.get("query")

let conn = open_fn(":memory:")
run(conn, "CREATE TABLE users (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT, age INTEGER)")
run(conn, "INSERT INTO users (name, age) VALUES (?, ?)", ["Alice", 30])

let rows = query(conn, "SELECT * FROM users WHERE age > ?", [25])
```

Build the SQLite extension with `make ext-sqlite`.

**Writing custom extensions:** compile a shared library against `lattice_ext.h` that exports a `lat_ext_init(LatExtContext *ctx)` function. Use `lat_ext_register()` to add functions, and the `lat_ext_*` helpers to construct and inspect values.

### ORM Library

Lattice includes a built-in ORM library (`lib/orm.lat`) that provides a simple object-relational mapping layer over SQLite with parameterized queries for safe database operations.

```lattice
import "lib/orm" as orm

let db = orm.connect(":memory:")

// Define a table schema
let schema = Map::new()
schema.set("id", "INTEGER PRIMARY KEY AUTOINCREMENT")
schema.set("name", "TEXT NOT NULL")
schema.set("age", "INTEGER")

let User = orm.model(db, "users", schema)

// Create the table
let create_table = User.get("create_table")
create_table(0)

// Insert records
let create = User.get("create")
let data = Map::new()
data.set("name", "Alice")
data.set("age", 30)
let id = create(data)

// Query records
let find = User.get("find")
let user = find(id)
print(user.get("name"))  // Alice

let where_fn = User.get("where")
let results = where_fn("age > ?", [25])

orm.close(db)
```

| Function | Description |
|----------|-------------|
| `orm.connect(path)` | Open an SQLite database, returns a db object |
| `orm.model(db, table, schema)` | Create a model for a table with the given schema |
| `orm.close(db)` | Close the database connection |

**Model methods** (accessed via `model.get("method_name")`):

| Method | Description |
|--------|-------------|
| `create_table(0)` | Create the table if it doesn't exist |
| `create(data)` | Insert a record (Map), returns the new row ID |
| `find(id)` | Find a record by ID, returns Map or nil |
| `all(0)` | Return all records as an Array of Maps |
| `where(condition, params)` | Query with a WHERE clause and `?` placeholders |
| `update(id, data)` | Update a record by ID with the given Map of fields |
| `delete(id)` | Delete a record by ID |
| `count(0)` | Return the number of records |
| `drop_table(0)` | Drop the table |

Requires the SQLite extension: `make ext-sqlite`.

### Standard Libraries

Lattice ships with importable libraries in the `lib/` directory:

| Library | Import | Description |
|---------|--------|-------------|
| Test Runner | `import "lib/test" as t` | Structured test suites with rich assertions |
| Validation | `import "lib/validate" as v` | Declarative schema validation for Maps |
| Functional | `import "lib/fn" as f` | Lazy sequences, Result type, currying, collection utilities |
| Dotenv | `import "lib/dotenv" as dotenv` | Load `.env` files into environment variables |
| CLI Parser | `import "lib/cli" as cli` | Command-line argument parsing with flags and subcommands |
| HTTP Server | `import "lib/http_server" as http` | HTTP request routing and response helpers |
| Logging | `import "lib/log" as log` | Leveled logging (debug/info/warn/error) |
| Template | `import "lib/template" as tmpl` | String template engine with `{{variable}}` interpolation |

Example using the test runner:

```lattice
import "lib/test" as t

t.run([
    t.describe("Math", |_| {
        return [
            t.it("addition", |_| {
                t.assert_eq(2 + 2, 4)
            }),
            t.it("division by zero", |_| {
                t.assert_throws(|_| { 1 / 0 })
            })
        ]
    })
])
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

Lattice ships with 120+ builtin functions and 70+ type methods covering I/O, math, strings, files, networking, data formats, concurrency, and more.

### Core

| Function | Description |
|----------|-------------|
| `print(args...)` | Print values to stdout with newline |
| `eprint(args...)` | Print to stderr with newline |
| `print_raw(args...)` | Print to stdout without newline |
| `input(prompt?)` | Read a line from stdin (Unit on EOF) |
| `typeof(val)` | Type name of a value (`"Int"`, `"String"`, etc.) |
| `phase_of(val)` | Phase of a value (`"fluid"`, `"crystal"`, `"sublimated"`, `"unphased"`) |
| `to_string(val)` | Convert any value to its string representation |
| `repr(val)` | Display representation (strings quoted, structs use custom `repr` if defined) |
| `len(val)` | Length of a string, array, or map |
| `assert(cond, msg?)` | Assert condition is truthy; error with optional message |
| `debug_assert(cond, msg?)` | Like `assert`, but skipped when `--no-assertions` is set |
| `exit(code?)` | Exit the process (default code 0) |
| `version()` | Lattice version string |

### Phase Transitions

| Function | Description |
|----------|-------------|
| `freeze(val)` | Transition a value to crystal (immutable) phase |
| `freeze(val) where \|v\| { ... }` | Freeze with validation contract |
| `freeze(val) except [fields]` | Freeze with field/key exemptions (defects) |
| `thaw(val)` | Create a mutable copy of a crystal or sublimated value |
| `clone(val)` | Deep-clone a value |
| `sublimate(val)` | Shallow freeze — locks structure, inner values stay mutable |
| `crystallize(var) { ... }` | Temporarily freeze for block duration, then auto-restore |
| `borrow(var) { ... }` | Temporarily thaw for block duration, then auto-restore |
| `bond(target, ...deps)` | Link variables for cascading freeze |
| `bond(target, dep, strategy)` | Bond with strategy: `"mirror"`, `"inverse"`, or `"gate"` |
| `unbond(target, ...deps)` | Remove a bond |
| `seed(var, contract)` | Attach a deferred freeze contract |
| `unseed(var)` | Remove a pending seed contract |
| `grow(var)` | Freeze + validate seed contract |
| `pressurize(var, mode)` | Restrict structural mutations (`"no_grow"`, `"no_shrink"`, `"no_resize"`) |
| `depressurize(var)` | Remove pressure constraint |
| `pressure_of(var)` | Query current pressure mode (nil if none) |
| `track(name)` | Enable phase history tracking for a variable |
| `phases(name)` | Get phase history as array of `{phase, value}` maps |
| `rewind(name, n)` | Get value from n steps back in history |
| `react(var, callback)` | Register a callback for phase transitions |
| `unreact(var)` | Remove all phase reaction callbacks |

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
| `struct_name(val)` | Returns the struct's type name as a String (e.g. `"User"`) |
| `struct_fields(val)` | Returns an array of field name strings |
| `struct_to_map(val)` | Converts a struct to a `{field_name: value}` Map |
| `struct_from_map(name, map)` | Creates a struct instance from a type name and Map of field values |

### Iterators

| Function | Description |
|----------|-------------|
| `iter(array)` | Create iterator from array |
| `range_iter(start, end)` | Lazy range iterator (optional step: `range_iter(0, 10, 2)`) |
| `repeat_iter(value)` | Infinite iterator repeating a value (optional count: `repeat_iter(0, 5)`) |

Iterator methods: `.map(fn)`, `.filter(fn)`, `.take(n)`, `.skip(n)`, `.enumerate()`, `.zip(other)`, `.collect()`, `.reduce(init, fn)`, `.any(fn)`, `.all(fn)`, `.count()`, `.next()`

### Testing

| Function | Description |
|----------|-------------|
| `assert_eq(a, b)` | Assert two values are equal |
| `assert_ne(a, b)` | Assert two values are not equal |
| `assert_true(x)` | Assert value is `true` |
| `assert_false(x)` | Assert value is `false` |
| `assert_nil(x)` | Assert value is `nil` |
| `assert_throws(fn)` | Assert closure throws an error |
| `assert_contains(haystack, needle)` | Assert string/array contains value |
| `assert_type(val, type_str)` | Assert value has the given type |

### Debugging

| Function | Description |
|----------|-------------|
| `breakpoint()` | Pause execution and drop into interactive REPL with access to locals, backtrace, and expression evaluation |

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
| `.capitalize()` | Capitalize first letter, lowercase the rest |
| `.title_case()` | Capitalize the first letter of each word |
| `.snake_case()` | Convert to snake_case |
| `.camel_case()` | Convert to camelCase |
| `.kebab_case()` | Convert to kebab-case |

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

### Iterator Methods

| Method | Description |
|--------|-------------|
| `.next()` | Get next value (nil when exhausted) |
| `.map(fn)` | Transform each element |
| `.filter(fn)` | Keep elements where fn returns true |
| `.take(n)` | Take first n elements |
| `.skip(n)` | Skip first n elements |
| `.enumerate()` | Yield `[index, value]` pairs |
| `.zip(other)` | Pair elements with another iterator |
| `.collect()` | Consume into array |
| `.reduce(init, fn)` | Fold into single value |
| `.any(fn)` | True if any element matches |
| `.all(fn)` | True if all elements match |
| `.count()` | Count remaining elements |

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
| Optional Chaining | `?.` `?[` |
| Result Propagation | `expr?` (unwrap ok or propagate err) |
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

The built-in REPL runs on the bytecode VM (same as file execution) and automatically displays expression results with a `=> ` prefix:

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

Assignments and statements that return Unit are silently suppressed. The REPL supports multi-line input (auto-detects incomplete expressions) and readline history. Globals, functions, structs, and enums defined in one line persist for subsequent lines.

Use `--tree-walk` to run the REPL on the tree-walking interpreter instead:

```sh
./clat --tree-walk
```

Lattice also includes a self-hosted REPL written in Lattice itself (`repl.lat`), built on `is_complete`, `lat_eval`, and `input`:

```sh
./clat repl.lat
```

## Bytecode VM

Lattice compiles source to a compact bytecode format and executes it on a stack-based VM with upvalue-based closures. The bytecode VM is the default execution mode for both file execution and the interactive REPL.

The VM supports the full phase system (freeze/thaw, react/bond/seed, sublimate, forge, anneal, pressure, alloy types), structs, enums, pattern matching, closures with captures, try/catch, defer, structured concurrency (`scope`/`spawn`/`select` with channels), and all builtin functions.

The tree-walking interpreter is available as a fallback via `--tree-walk`.

### Bytecode Compilation (.latc)

Lattice can compile source files to standalone `.latc` bytecode files using the C compiler backend:

```sh
./clat compile program.lat -o program.latc   # compile to bytecode
./clat program.latc                           # run compiled bytecode
```

A self-hosted compiler written in Lattice itself (`compiler/latc.lat`) can also produce `.latc` files:

```sh
./clat compiler/latc.lat program.lat output.latc   # compile via self-hosted compiler
./clat output.latc                                  # run the result
```

Both backends produce identical output. The self-hosted compiler supports the full language including structs, enums, traits/impl, closures, match expressions, destructuring, phase semantics (fix/freeze/thaw/clone), and nil coalescing.

## CLI Reference

```
clat [options] [file.lat]
clat compile <file.lat> -o <output.latc>
clat fmt [--check] [--stdin] <file.lat>
clat doc [--json|--html] [-o dir] <file.lat>
clat test [--filter pat] [--verbose] [--summary] <file_or_dir>
```

| Flag | Description |
|------|-------------|
| `file.lat` | Run a Lattice source file (or `.latc` bytecode file) |
| *(no file)* | Start the interactive REPL (with tab completion) |
| `compile file.lat -o out.latc` | Compile source to bytecode file |
| `fmt file.lat` | Format source code (4-space indent, attached braces) |
| `fmt --check file.lat` | Check formatting without modifying (exit 1 if unformatted) |
| `doc file.lat` | Generate documentation from `///` doc comments |
| `test file_or_dir` | Run tests with assertion builtins and colored output |
| `--tree-walk` | Use the tree-walking interpreter instead of the bytecode VM |
| `--regvm` | Use the register-based VM backend |
| `--debug` | Run with interactive debugger (step/next/continue/breakpoints) |
| `--break N` | Set initial breakpoint at line N (implies `--debug`) |
| `--gc` | Enable opt-in mark-sweep garbage collector |
| `--gc-stress` | Force garbage collection on every allocation (for testing) |
| `--stats` | Print memory/GC statistics to stderr after execution (tree-walk mode) |
| `--no-assertions` | Disable `debug_assert()` and `require`/`ensure` contracts |

## Building & Testing

```sh
make            # build the clat binary
make test       # run the test suite
make asan       # build and test with AddressSanitizer + UBSan
make tsan       # build and test with ThreadSanitizer
make coverage   # generate code coverage report (build/coverage/index.html)
make analyze    # run clang static analyzer
make fuzz       # build the libFuzzer harness (requires Homebrew LLVM)
make fuzz-seed  # seed the fuzz corpus from examples/ and benchmarks/
make ext-pg     # build the PostgreSQL extension (requires libpq)
make ext-sqlite # build the SQLite extension (requires libsqlite3)
make clean      # remove build artifacts
```

**Required:** libedit — ships with macOS. On Linux, install `libedit-dev` (Debian/Ubuntu) or `libedit-devel` (Fedora/RHEL).

**Optional:** OpenSSL — enables TLS networking (`tls_*`) and crypto builtins (`sha256`, `md5`, `base64_*`). Detected automatically via pkg-config. Build without it using `make TLS=0`.

## License

BSD 3-Clause License. Copyright (c) 2026, Alex Jokela.

See [LICENSE](LICENSE) for the full text.
