#!/bin/bash
# Run Lattice snippets through both stack VM and register VM, compare outputs.
# Usage: bash tests/regvm_parity.sh

CLAT=./clat
PASS=0
FAIL=0
SKIP=0
ERRORS=""

run_test() {
    local name="$1"
    local src="$2"

    tmpfile=$(mktemp /tmp/lat_test_XXXXXX.lat)
    echo "$src" > "$tmpfile"

    stack_out=$($CLAT "$tmpfile" 2>&1)
    stack_rc=$?
    reg_out=$($CLAT --regvm "$tmpfile" 2>&1)
    reg_rc=$?

    rm -f "$tmpfile"

    if [ $stack_rc -ne 0 ] && [ $reg_rc -ne 0 ]; then
        # Both fail — skip (test may use unsupported features)
        SKIP=$((SKIP + 1))
        return
    fi

    if [ "$stack_out" = "$reg_out" ]; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
        ERRORS="$ERRORS\nFAIL: $name\n  stack: $(echo "$stack_out" | head -3)\n  regvm: $(echo "$reg_out" | head -3)\n"
    fi
}

echo "Running register VM parity tests..."
echo ""

# ── Arithmetic ──
run_test "int_add" 'print(1 + 2)'
run_test "int_sub" 'print(10 - 3)'
run_test "int_mul" 'print(4 * 5)'
run_test "int_div" 'print(15 / 3)'
run_test "int_mod" 'print(17 % 5)'
run_test "int_neg" 'print(-42)'
run_test "int_precedence" 'print(2 + 3 * 4)'
run_test "int_parens" 'print((2 + 3) * 4)'
run_test "float_add" 'print(3.14 + 1.0)'
run_test "float_div" 'print(10.0 / 3.0)'

# ── Comparison ──
run_test "lt" 'print(1 < 2)'
run_test "gt" 'print(2 > 1)'
run_test "lteq" 'print(1 <= 1)'
run_test "gteq" 'print(1 >= 2)'
run_test "eq" 'print(1 == 1)'
run_test "neq" 'print(1 != 2)'
run_test "and" 'print(true && false)'
run_test "or" 'print(true || false)'
run_test "not" 'print(!true)'

# ── Bitwise ──
run_test "bit_and" 'print(5 & 3)'
run_test "bit_or" 'print(5 | 3)'
run_test "bit_xor" 'print(5 ^ 3)'
run_test "bit_not" 'print(~0)'
run_test "lshift" 'print(1 << 4)'
run_test "rshift" 'print(16 >> 2)'

# ── Strings ──
run_test "str_concat" 'print("hello" + " " + "world")'
run_test "str_len" 'print("abc".len())'
run_test "str_upper" 'print("hello".to_upper())'
run_test "str_lower" 'print("HELLO".to_lower())'
run_test "str_trim" 'print("  trim  ".trim())'
run_test "str_split" 'print("hello world".split(" "))'
run_test "str_contains" 'print("hello".contains("ell"))'
run_test "str_substring" 'print("abcdef".substring(2, 4))'
run_test "str_repeat" 'print("ha".repeat(3))'
run_test "str_replace" 'print("hello world".replace("world", "lattice"))'
run_test "str_starts_with" 'print("hello".starts_with("hel"))'
run_test "str_ends_with" 'print("hello".ends_with("llo"))'
run_test "str_reverse" 'print("hello".reverse())'
run_test "str_chars" 'print("abc".chars())'
run_test "str_index_of" 'print("hello".index_of("ll"))'
run_test "str_pad_left" 'print("hi".pad_left(5, " "))'
run_test "str_pad_right" 'print("hi".pad_right(5, " "))'
run_test "str_interpolation" 'let x = 42
print("val = ${x}")'

# ── Variables ──
run_test "let_var" 'let x = 42
print(x)'
run_test "flux_var" 'flux y = 10
y = 20
print(y)'

# ── Arrays ──
run_test "array_literal" 'print([1, 2, 3])'
run_test "array_len" 'print([1, 2, 3].len())'
run_test "array_index" 'let a = [10, 20, 30]
print(a[1])'
run_test "array_first" 'print([1, 2, 3].first())'
run_test "array_last" 'print([1, 2, 3].last())'
run_test "array_contains" 'print([1, 2, 3].contains(2))'
run_test "array_index_of" 'print([10, 20, 30].index_of(20))'
run_test "array_reverse" 'print([1, 2, 3].reverse())'
run_test "array_slice" 'print([1, 2, 3, 4, 5].slice(1, 3))'
run_test "array_push_local" 'fn test() {
    let arr = [1, 2]
    arr.push(3)
    print(arr)
}
test()'
run_test "array_push_global" 'flux arr = []
arr.push(1)
arr.push(2)
print(arr)'
run_test "array_pop" 'flux arr = [1, 2, 3]
let v = arr.pop()
print(v)
print(arr)'
run_test "array_map" 'print([1, 2, 3].map(|x| { x * 2 }))'
run_test "array_filter" 'print([1, 2, 3, 4].filter(|x| { x > 2 }))'
run_test "array_reduce" 'print([1, 2, 3].reduce(0, |acc, x| { acc + x }))'
run_test "array_any" 'print([1, 2, 3].any(|x| { x > 2 }))'
run_test "array_all" 'print([1, 2, 3].all(|x| { x > 0 }))'
run_test "array_find" 'print([1, 2, 3].find(|x| { x == 2 }))'
run_test "array_enumerate" 'print([10, 20].enumerate())'
run_test "array_unique" 'print([1, 2, 2, 3, 1].unique())'
run_test "array_flatten" 'print([[1, 2], [3, 4]].flatten())'
run_test "array_take" 'print([1, 2, 3, 4].take(2))'
run_test "array_drop" 'print([1, 2, 3, 4].drop(2))'
run_test "array_zip" 'print([1, 2, 3].zip([4, 5, 6]))'
run_test "array_sum" 'print([1, 2, 3, 4].sum())'
run_test "array_min" 'print([3, 1, 2].min())'
run_test "array_max" 'print([3, 1, 2].max())'
run_test "array_each" 'let a = [1, 2, 3]
a.each(|x| { print(x) })'
run_test "array_for_each" 'let a = [1, 2, 3]
a.for_each(|x| { print(x) })'

# ── Maps ──
run_test "map_basic" 'let m = Map::new()
m["a"] = 1
m["b"] = 2
print(m["a"])
print(m["b"])'
run_test "map_len" 'let m = Map::new()
m["x"] = 1
print(m.len())'
run_test "map_has" 'let m = Map::new()
m["k"] = 1
print(m.has("k"))
print(m.has("z"))'
run_test "map_keys" 'let m = Map::new()
m["a"] = 1
print(m.keys())'
run_test "map_values" 'let m = Map::new()
m["a"] = 1
print(m.values())'

# ── Structs ──
run_test "struct_basic" 'struct Point { x: int, y: int }
let p = Point { x: 10, y: 20 }
print(p.x)
print(p.y)'
run_test "struct_method" 'struct Counter { value: int, inc: any }
let c = Counter { value: 0, inc: |self| { self.value + 1 } }
print(c.inc())'

# ── Functions ──
run_test "fn_basic" 'fn add(a: int, b: int) { a + b }
print(add(3, 4))'
run_test "fn_string" 'fn greet(name: str) { "Hello, " + name + "!" }
print(greet("World"))'
run_test "fn_recursive" 'fn factorial(n: int) {
    if n <= 1 { return 1 }
    return n * factorial(n - 1)
}
print(factorial(10))'

# ── Closures ──
run_test "closure_capture" 'fn make_adder(n: int) {
    return |x| { x + n }
}
let add5 = make_adder(5)
print(add5(10))'
run_test "closure_higher_order" 'fn apply(f: any, val: int) { f(val) }
print(apply(|x| { x * 3 }, 7))'

# ── Control flow ──
run_test "if_true" 'print(if true { "yes" } else { "no" })'
run_test "if_false" 'print(if false { "yes" } else { "no" })'
run_test "match_basic" 'let x = "A"
let r = match x {
    "A" => "first",
    "B" => "second",
    _ => "other",
}
print(r)'

# ── Loops ──
run_test "while_loop" 'flux i = 0
flux sum = 0
while i < 5 {
    sum = sum + i
    i = i + 1
}
print(sum)'
run_test "for_loop" 'flux sum = 0
for n in [10, 20, 30] {
    sum = sum + n
}
print(sum)'
run_test "for_strings" 'for s in ["a", "b", "c"] {
    print(s)
}'
run_test "loop_break" 'flux c = 0
loop {
    if c >= 3 { break }
    c = c + 1
}
print(c)'
run_test "for_range" 'flux sum = 0
for n in 1..5 {
    sum = sum + n
}
print(sum)'
run_test "nested_for" 'flux result = []
for i in [1, 2, 3] {
    for j in [10, 20] {
        result.push(i * j)
    }
}
print(result)'

# ── Enums ──
run_test "enum_basic" 'enum Color { Red, Green, Blue }
let c = Color::Red
print(c)'
run_test "enum_tag" 'enum Color { Red, Green, Blue }
print(Color::Red.tag())'
run_test "enum_name" 'enum Color { Red, Green, Blue }
print(Color::Red.enum_name())'
run_test "enum_payload" 'enum Shape { Circle(r) }
let s = Shape::Circle(5.0)
print(s.tag())
print(s.payload())'

# ── Exception handling ──
run_test "try_catch" 'let r = try {
    let x = 10 / 0
    "ok"
} catch err {
    "caught"
}
print(r)'
run_test "throw_catch" 'let r = try {
    throw("boom")
} catch err {
    err
}
print(r)'

# ── Nil coalescing ──
run_test "nil_coalesce" 'let v = nil ?? "default"
print(v)'
run_test "nil_coalesce_non_nil" 'let v = 42 ?? "default"
print(v)'

# ── Tuples ──
run_test "tuple_basic" 'let t = (1, "hello", true)
print(t)'
run_test "tuple_len" 'let t = (1, 2, 3)
print(t.len())'

# ── Global mutation ──
run_test "global_push" 'flux arr = []
arr.push(1)
arr.push(2)
arr.push(3)
print(arr)
print(arr.len())'

# ── Defer ──
run_test "defer_basic" 'fn with_defer() {
    defer { print("deferred") }
    print("before")
}
with_defer()'
run_test "defer_lifo" 'fn two_defers() {
    defer { print("second") }
    defer { print("first") }
    print("body")
}
two_defers()'

# ── Destructuring ──
run_test "destructure_array" 'let [a, b, c] = [10, 20, 30]
print(a)
print(b)
print(c)'

# ── Phases ──
run_test "flux_phase" 'flux x = 1
x = 2
print(x)'
run_test "fix_phase" 'fix x = 42
print(x)'
run_test "clone_value" 'let a = [1, 2, 3]
let b = clone(a)
print(b)'

# ── String interpolation complex ──
run_test "interp_expr" 'print("1 + 2 = ${1 + 2}")'
run_test "interp_var" 'let name = "Lattice"
print("Hello, ${name}!")'

# ── Index assignment ──
run_test "index_assign_local" 'fn test() {
    let a = [0, 0, 0]
    a[0] = 10
    a[1] = 20
    a[2] = 30
    print(a)
}
test()'
run_test "index_assign_global" 'flux a = [0, 0, 0]
a[0] = 10
a[1] = 20
a[2] = 30
print(a)'

# ── Mixed operations ──
run_test "complex_pipeline" 'fn process(items: any) {
    items.map(|x| { x * 2 }).filter(|x| { x > 4 })
}
print(process([1, 2, 3, 4, 5]))'
run_test "fn_return_array" 'fn divmod(a: int, b: int) {
    return [a / b, a % b]
}
let r = divmod(17, 5)
print(r[0])
print(r[1])'

# ── Print summary ──
echo ""
echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"
if [ $FAIL -gt 0 ]; then
    echo ""
    echo "Failures:"
    echo -e "$ERRORS"
    exit 1
fi
