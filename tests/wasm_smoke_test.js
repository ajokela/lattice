/**
 * WASM smoke tests for Lattice
 *
 * Loads the Emscripten-generated lattice.js module and exercises
 * the exported REPL API: lat_init, lat_run_line, lat_is_complete,
 * lat_get_error, lat_clear_error, lat_destroy, and the RegVM
 * equivalents.
 *
 * Exit code: 0 on all-pass, 1 on any failure.
 */

"use strict";

const path = require("path");
const fs = require("fs");

/* ---------- locate the WASM module ---------- */

// Try build/ first (CI override), then lattice-lang.org/ (dev default)
const candidates = [
  path.resolve(__dirname, "..", "build", "lattice.js"),
  path.resolve(__dirname, "..", "lattice-lang.org", "lattice.js"),
];
let modulePath = null;
for (const p of candidates) {
  if (fs.existsSync(p)) {
    modulePath = p;
    break;
  }
}
if (!modulePath) {
  console.error("ERROR: Could not find lattice.js in any expected location:");
  for (const p of candidates) console.error("  - " + p);
  process.exit(1);
}

/* ---------- test harness ---------- */

let passed = 0;
let failed = 0;
const failures = [];

function assert(cond, msg) {
  if (!cond) {
    failed++;
    failures.push(msg);
    console.log("  FAIL: " + msg);
  } else {
    passed++;
    console.log("  PASS: " + msg);
  }
}

function assertIncludes(haystack, needle, msg) {
  assert(haystack.includes(needle), msg + ' (expected "' + needle + '" in output)');
}

/* ---------- main ---------- */

(async () => {
  /* Capture stdout/stderr from the WASM module */
  let outputLines = [];
  let errorLines = [];

  const createLattice = require(modulePath);

  const Module = await createLattice({
    print: (text) => {
      outputLines.push(text);
    },
    printErr: (text) => {
      errorLines.push(text);
    },
  });

  /* Wrap the exported C functions */
  const latInit = Module._lat_init;
  const latRunLine = Module.cwrap("lat_run_line", "string", ["string"]);
  const latIsComplete = Module.cwrap("lat_is_complete", "number", ["string"]);
  const latGetError = Module.cwrap("lat_get_error", "string", []);
  const latClearError = Module._lat_clear_error;
  const latDestroy = Module._lat_destroy;
  const latHeapBytes = Module._lat_heap_bytes;

  const latInitRegvm = Module._lat_init_regvm;
  const latRunLineRegvm = Module.cwrap("lat_run_line_regvm", "string", [
    "string",
  ]);
  const latDestroyRegvm = Module._lat_destroy_regvm;

  /* Helper: run a line and return { output, errors } */
  function run(line) {
    outputLines = [];
    errorLines = [];
    latRunLine(line);
    return { output: outputLines.join("\n"), errors: errorLines.join("\n") };
  }

  function runRegvm(line) {
    outputLines = [];
    errorLines = [];
    latRunLineRegvm(line);
    return { output: outputLines.join("\n"), errors: errorLines.join("\n") };
  }

  /* ================================================================
   *  StackVM tests
   * ================================================================ */

  console.log("\n=== StackVM Tests ===\n");
  latInit();

  /* -- Arithmetic -- */
  console.log("[Arithmetic]");
  {
    const r = run("2 + 3");
    assertIncludes(r.output, "5", "integer addition");
  }
  {
    const r = run("10 * 4 - 7");
    assertIncludes(r.output, "33", "mixed arithmetic");
  }
  {
    const r = run("3.14 + 1.0");
    assert(
      r.output.includes("4.14"),
      "float addition"
    );
  }

  /* -- Strings -- */
  console.log("[Strings]");
  {
    const r = run('"hello" + " " + "world"');
    assertIncludes(r.output, "hello world", "string concatenation");
  }
  {
    const r = run('"hello".len()');
    assertIncludes(r.output, "5", "string length");
  }

  /* -- Booleans -- */
  console.log("[Booleans]");
  {
    const r = run("true && false");
    assertIncludes(r.output, "false", "boolean AND");
  }
  {
    const r = run("true || false");
    assertIncludes(r.output, "true", "boolean OR");
  }
  {
    const r = run("!true");
    assertIncludes(r.output, "false", "boolean NOT");
  }

  /* -- Variables & phases -- */
  console.log("[Variables & Phases]");
  {
    run("flux x = 42");
    const r = run("x");
    assertIncludes(r.output, "42", "flux variable");
  }
  {
    const r = run("phase_of(x)");
    assertIncludes(r.output, "fluid", "phase_of flux var");
  }

  /* -- Functions -- */
  console.log("[Functions]");
  {
    run('fn add(a: Int, b: Int) -> Int { a + b }');
    const r = run("add(10, 20)");
    assertIncludes(r.output, "30", "function definition and call");
  }
  {
    run("let mul = |a, b| { a * b }");
    const r = run("mul(6, 7)");
    assertIncludes(r.output, "42", "closure / lambda");
  }

  /* -- Arrays -- */
  console.log("[Arrays]");
  {
    run("flux arr = [1, 2, 3]");
    const r = run("arr.len()");
    assertIncludes(r.output, "3", "array length");
  }
  {
    run("arr.push(4)");
    const r = run("arr");
    assertIncludes(r.output, "4", "array push");
  }

  /* -- Structs -- */
  console.log("[Structs]");
  {
    run("struct Point { x: Int, y: Int }");
    run("let p = Point { x: 3, y: 4 }");
    const r = run("p.x + p.y");
    assertIncludes(r.output, "7", "struct field access");
  }

  /* -- Control flow -- */
  console.log("[Control Flow]");
  {
    const r = run('if true { "yes" } else { "no" }');
    assertIncludes(r.output, "yes", "if-else true branch");
  }
  {
    const r = run('if false { "yes" } else { "no" }');
    assertIncludes(r.output, "no", "if-else false branch");
  }

  /* -- Print function -- */
  console.log("[Print]");
  {
    const r = run('print("hello from wasm")');
    assertIncludes(r.output, "hello from wasm", "print function");
  }

  /* -- Match expression -- */
  console.log("[Match]");
  {
    const r = run('match 42 { 1 => "one", 42 => "forty-two", _ => "other" }');
    assertIncludes(r.output, "forty-two", "match expression");
  }

  /* -- Error handling -- */
  console.log("[Error Handling]");
  {
    const r = run('try { 10 / 0 } catch e { "caught" }');
    assertIncludes(r.output, "caught", "try-catch division by zero");
  }

  /* -- lat_is_complete -- */
  console.log("[Completeness Check]");
  {
    assert(latIsComplete("1 + 2") === 1, "complete expression");
    assert(latIsComplete("fn foo() {") === 0, "incomplete: open brace");
    assert(latIsComplete("fn foo() { 1 }") === 1, "complete function");
  }

  /* -- lat_get_error / lat_clear_error -- */
  console.log("[Error API]");
  {
    latClearError();
    run("@@@invalid_syntax@@@");
    const err = latGetError();
    assert(err !== null && err.length > 0, "lat_get_error returns error after bad input");
    latClearError();
    const cleared = latGetError();
    assert(cleared === null || cleared === "", "lat_clear_error clears error");
  }

  /* -- Heap bytes -- */
  console.log("[Heap Info]");
  {
    const bytes = latHeapBytes();
    assert(bytes > 0, "lat_heap_bytes returns positive value");
  }

  /* Clean up StackVM */
  latDestroy();

  /* ================================================================
   *  RegVM tests
   * ================================================================ */

  console.log("\n=== RegVM Tests ===\n");
  latInitRegvm();

  /* -- Basic arithmetic on RegVM -- */
  console.log("[RegVM Arithmetic]");
  {
    const r = runRegvm("2 + 3");
    assertIncludes(r.output, "5", "regvm integer addition");
  }
  {
    const r = runRegvm("10 * 4 - 7");
    assertIncludes(r.output, "33", "regvm mixed arithmetic");
  }

  /* -- Strings on RegVM -- */
  console.log("[RegVM Strings]");
  {
    const r = runRegvm('"foo" + "bar"');
    assertIncludes(r.output, "foobar", "regvm string concatenation");
  }

  /* -- Functions on RegVM -- */
  console.log("[RegVM Functions]");
  {
    runRegvm("fn square(n: Int) -> Int { n * n }");
    const r = runRegvm("square(9)");
    assertIncludes(r.output, "81", "regvm function call");
  }

  /* -- Print on RegVM -- */
  console.log("[RegVM Print]");
  {
    const r = runRegvm('print("regvm hello")');
    assertIncludes(r.output, "regvm hello", "regvm print");
  }

  /* Clean up RegVM */
  latDestroyRegvm();

  /* ================================================================
   *  Summary
   * ================================================================ */

  console.log("\n=== Results ===");
  console.log("Passed: " + passed);
  console.log("Failed: " + failed);

  if (failures.length > 0) {
    console.log("\nFailures:");
    for (const f of failures) {
      console.log("  - " + f);
    }
  }

  process.exit(failed > 0 ? 1 : 0);
})().catch((err) => {
  console.error("Fatal error:", err);
  process.exit(1);
});
