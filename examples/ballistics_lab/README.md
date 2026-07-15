# Ballistics laboratory

This directory is the source-level foundation for a ballistics laboratory
written in ordinary Lattice. It does not add a ballistics intrinsic, native
extension, or C ABI dependency to the language runtime.

The modules are deliberately split at stable boundaries:

- units.lat defines distinct SI-backed quantities while retaining preferred
  display values and units.
- domain.lat defines validated, immutable projectile, rifle, atmosphere,
  wind, shot, solver, experiment, result, notice, and error values.
- protocol_v1.lat explicitly converts an Experiment into the
  ballistics-engine solve-json v1 Map shape.
- backend.lat defines the transport-independent EngineBackend closure contract.
- process_backend.lat invokes an explicitly configured engine with shell-free
  argv/stdin transport and strictly decodes solve-json v1 responses.
- analysis.lat provides checked trajectory interpolation and sampling plus
  programmable DOPE, wind-card, and multi-trajectory comparison tables.
- analysis_export.lat converts those tables to explicit Maps, canonical JSON,
  rectangular CSV, or deterministic plain text without performing I/O.
- session.lat owns mutable experiment state, display preferences, transactional
  solving, and bounded immutable run history independently of terminal input.
- persistence.lat saves and loads strict versioned experiment JSON through
  explicit domain adapters, without serializing executable or session state.
- resolved_request_v1.lat validates detached solve-json v1 request snapshots.
- verified_artifacts.lat deliberately promotes completed run records into
  immutable, portable verified-run documents.
- render.lat returns deterministic metric or imperial terminal strings for
  experiments, runs, observations, analysis tables, notices, and errors.
- command_parser.lat parses the bounded colon-command grammar as inert data.
- commands.lat dispatches parsed commands directly to the session, analysis,
  persistence, and rendering APIs without generating source or shell text.
- repl_support.lat defines the pure Nil/legacy-Unit EOF compatibility check.
- reference_experiment.lat is an executable construction example.

The process backend is process-per-solve. It retains no engine handle, performs
no network access, and does no executable discovery of its own. The caller
configures the executable; when that value is a bare name, the operating system
may resolve it through `PATH`. Packaging-level discovery is intentionally
separate.

## Example

    import "examples/ballistics_lab/units" as u
    import "examples/ballistics_lab/domain" as b
    import "examples/ballistics_lab/protocol_v1" as protocol

    let load = b.projectile(
        "175 gr match",
        u.gr(175),
        u.inches(0.308),
        0.243,
        "G7",
        u.inches(1.24)
    )

    let rifle = b.rifle(
        "match rifle",
        u.inches(1.5),
        u.m(0),
        u.inches(8),
        "right"
    )

    let atmosphere = b.atmosphere(
        u.m(250),
        u.celsius(15),
        u.hpa(1013.25),
        0.5,
        u.deg(45)
    )

    let shot = b.shot(u.fps(2700), u.yd(1000), u.yd(100))
    let solver = b.solver_config("rk45", nil, u.yd(10))
    let experiment = b.experiment(
        "1,000-yard match",
        load,
        rifle,
        atmosphere,
        [b.wind(u.mph(10), u.deg(90))],
        shot,
        solver
    )

    let request = protocol.experiment_to_v1_request(experiment)
    print(json_stringify(request))

Canonical fields use meters, meters per second, kilograms, radians, kelvin,
pascals, and joules. Display metadata never enters the protocol Map. A
ballistic mil is exactly one milliradian (0.001 rad).

## Solving through the process backend

Supply the engine executable explicitly and handle the typed result:

    import "examples/ballistics_lab/backend" as backend
    import "examples/ballistics_lab/process_backend" as process

    let engine = process.process_backend("/opt/ballistics/bin/ballistics")
    let result = backend.solve_with(engine, experiment)

    if backend.solve_succeeded(result) {
        print("engine=" + result.engine_version)
        print("range_m=" + to_string(result.summary.actual_range.si_value))
        print("samples=" + to_string(len(result.observations)))
    } else {
        print(result.code + ": " + result.message)
    }

`solve_with` returns either an immutable `Trajectory` or an immutable
`LaboratoryError`; ordinary protocol failures are values rather than uncaught
runtime errors. A trajectory retains the original experiment, the validated
and frozen resolved-request Map, schema and engine versions, assumptions,
warnings, samples, summary, and termination reason.

The schema version is the compatibility boundary and must be exactly `1`.
`engine_version` is an opaque non-empty implementation identifier. Reproducible
workflows may pin it exactly:

    let engine = process.process_backend(
        "/opt/ballistics/bin/ballistics",
        "0.24.1"
    )

An omitted pin accepts any engine that emits a valid v1 envelope. It does not
silently accept another schema version.

## Querying and comparing trajectories

Analysis is ordinary Lattice over immutable domain values. A query keeps the
caller's distance display unit, checks the computed range, and interpolates
only between adjacent engine samples:

    import "examples/ballistics_lab/analysis" as analysis
    import "examples/ballistics_lab/analysis_export" as export
    import "examples/ballistics_lab/units" as u

    let point = analysis.trajectory_at(result, u.yd(300))
    let grid = analysis.sample(result, u.yd(100), u.yd(600), u.yd(25))
    let dope = analysis.dope(
        result,
        [u.yd(100), u.yd(200), u.yd(300)],
        "in",
        "moa",
        "ft/s",
        "ft-lb"
    )

    print(export.render_table(dope))
    let json = export.table_to_json(dope)
    let csv = export.table_to_csv(dope)

`sample` clips a requested stop to an early engine termination and includes the
real terminal observation exactly once. Results are capped at 10,000 rows.
`wind_card` accepts one or more calm/constant-wind trajectories;
`compare_trajectories` accepts at least two trajectories and aligns mismatched
engine grids on their sorted union within the shared computed range. Positive
drop/windage follow engine coordinates; come-up and wind-hold are the opposite
angular corrections.

Every `AnalysisTable` is immutable and carries ordered columns, scalar row
Maps, and per-trajectory provenance: solve schema and engine version, resolved
solver method, assumptions, warnings, verification state, termination reason,
and actual computed range. Calculation does not print or write. Export and
render functions return values or strings so callers retain control of I/O.

## Stateful laboratory sessions

`LabSession` keeps the current immutable experiment, backend configuration,
display preferences, history limit, and successful run records behind one
Lattice `Ref`. Session operations validate and construct a complete next state
before publishing it with one `Ref.set`, so a failed mutation or solve leaves
the prior state unchanged:

    import "examples/ballistics_lab/session" as session

    let lab = session.new_session(experiment, engine)
    let run = session.solve_session(lab)

    if struct_name(run) == "RunRecord" {
        print(run.engine_version)
        print(session.current_run(lab).termination)
    } else {
        print(run.code + ": " + run.message)
    }

Use `replace_projectile`, `replace_rifle`, `replace_atmosphere`,
`replace_winds`, `replace_shot`, `replace_solver`, or `replace_experiment` for
validated experiment changes. Each operation reconstructs the whole immutable
`Experiment`, preserving relational checks. `set_backend`,
`set_display_preferences`, and `set_history_limit` update other session state;
shrinking a history limit retains the newest records, and a limit of zero
disables history without disabling current/previous run tracking.

`solve_session` snapshots the submitted experiment and invokes the backend
exactly once. Success returns an immutable `RunRecord` and atomically advances
`previous_run`, `current_run`, and bounded history. A declared, thrown, or
contract-invalid backend failure returns `LaboratoryError` without changing
experiment or run state. `reset_session` clears current, previous, and history
while retaining experiment, backend, display preferences, and history limit.
The session stores backend configuration but no child-process handle; the
process backend remains process-per-solve and stateless between calls.

## Persisting experiments and verified runs

Experiment documents contain only explicit domain data and display-unit
metadata. They never contain backend closures, child-process state, session
history, resolved requests, or executable Lattice source:

    import "examples/ballistics_lab/persistence" as persistence

    persistence.save_experiment("match-load.json", experiment)
    let restored = persistence.load_experiment("match-load.json")
    persistence.load_experiment_into_session(lab, "match-load.json")

The document kind is `lattice-ballistics/experiment` and its schema version is
exactly `1`. Loading rejects malformed JSON, duplicate or unknown fields,
unsupported versions and units, non-finite values, and domain-invalid graphs.
The complete experiment is validated before a session is changed; a failed load
leaves all session state untouched. A round trip preserves both the solve-json
v1 request and the user's display values and units.

A successful `RunRecord` remains working session data. Promote it explicitly
when it has been reviewed:

    import "examples/ballistics_lab/verified_artifacts" as artifacts

    let verified = artifacts.verify_run(
        session.current_run(lab),
        [0, 10, 20],
        "Reviewed against the reference load."
    )
    artifacts.save_verified_run("reviewed-run.json", verified)
    let restored_verified = artifacts.load_verified_run("reviewed-run.json")

`VerifiedRun` is a distinct frozen value. It contains a detached experiment and
resolved-request snapshot, backend/schema/engine versions, assumptions,
warnings, summary, selected observations, termination, and an optional note.
Its strict `lattice-ballistics/verified-run` v1 document contains only JSON data;
serializers reject ordinary run records, draft trajectories, and legacy
trajectory verification flags. Loaded artifacts are fully validated and frozen
again. No persistence API generates or evaluates `.lat` source.

## Terminal rendering and colon commands

Terminal rendering is separate from calculation and export. Every renderer
returns a `String`; callers decide whether to print it, save it, or ignore it:

    import "examples/ballistics_lab/render" as render

    let metric = render.metric_render_profile(2)
    let text = render.render_observation(observation, metric)

Metric and imperial profiles use fixed decimal formatting, explicit trajectory
sign conventions, declared table-column order, and aligned comparison rows.
CSV and JSON remain in `analysis_export.lat`; terminal formatting never changes
the programmable analysis values.

The command layer is intentionally small:

    :help
    :show
    :solve
    :at <distance> <m|km|yd|ft|in>
    :dope <start> <stop> <interval> <unit>
    :wind-card <start> <stop> <interval> <unit>
    :compare
    :save "<path>"
    :load "<path>"
    :reset
    :quit

Parse and dispatch without evaluating generated Lattice:

    import "examples/ballistics_lab/command_parser" as parser
    import "examples/ballistics_lab/commands" as commands

    let command = parser.parse_command(":at 300 yd")
    let response = if parser.parse_failed(command) {
        command
    } else {
        commands.dispatch_command(lab, command)
    }

    if commands.command_succeeded(response) {
        print(response.output)
    } else {
        print(render.render_error(response))
    }

Quoted paths, including Windows paths, remain inert string arguments. The
parser never invokes the Lattice lexer, evaluator, a shell, or a process.
Invalid syntax, invalid arguments, unavailable run state, analysis failures,
backend errors, and failed loads return structured `LaboratoryError` values.
They do not mutate the laboratory session. Rifle, projectile, atmosphere, and
other experiment changes remain visible ordinary Lattice calls rather than a
second `:set` language. Command input is capped at 4,096 bytes and 16 arguments;
control characters and non-finite numeric arguments are rejected.

## Self-hosted terminal REPL

Start the laboratory directly with `clat`; no native extension or `--prelude`
flag is required:

    ./clat examples/ballistics_lab/ballistics-repl.lat \
        /absolute/path/to/ballistics

When the executable argument is omitted, the application uses a non-empty
`BALLISTICS_ENGINE` environment value and otherwise the bare name
`ballistics`. Set `BALLISTICS_ENGINE_VERSION` to require an exact engine
version. The configured executable is not started until `:solve`.

The initial `lab` contains the representative 175 gr experiment. Unit and
domain constructors, along with the `units`, `domain`, `sessions`, `analysis`,
`persistence`, `artifacts`, `render`, and `commands` module aliases, are in the
evaluator prelude. Ordinary input is persistent Lattice, so an experiment can
be inspected and replaced without a second configuration language:

    ballistics> let base = sessions.experiment_of(lab)
    ballistics> let alternate_projectile = projectile("alternate 175 gr", gr(175), inches(0.308), 0.245, "G7", inches(1.24))
    ballistics> let alternate = experiment("alternate load", alternate_projectile, base.rifle, base.atmosphere, base.winds, base.shot, base.solver)
    ballistics> sessions.replace_experiment(lab, alternate)

Expressions, declarations, functions, and multiline blocks persist for later
input. A reference workflow is:

    ballistics> :solve
    ballistics> :at 300 yd
    ballistics> :dope 100 600 100 yd
    ballistics> :wind-card 100 600 100 yd
    ballistics> :save "alternate-load.json"
    ballistics> :load "alternate-load.json"

For a comparison, solve one experiment, replace the experiment through the
source API, solve again, and run `:compare`; it compares the previous and
current successful runs. Parse, evaluation, command, and engine failures are
reported without ending the session, so corrected input can be entered next.
Use `:quit` or Ctrl-D to exit.

The v1 application deliberately uses the portable `input()` loop. It does not
yet provide native line editing, command history, or completion. It is not
available in browser/WASM builds because terminal input and direct process
execution are unsupported there.

## Resource bounds

`process_backend()` uses `exec_argv`, sends the request on stdin, and passes only
the literal `solve-json` argument. User experiment values never enter argv or a
shell command string. The lower-level `process_backend_with_argv()` seam accepts
an explicit argv array for adapters and tests, but still performs direct process
execution without a shell. Defaults are:

- 30 second child-process wall-clock timeout (response decoding is separate);
- 8 MiB captured stdout;
- 256 KiB captured stderr;
- the protocol's 1 MiB request and 10,000-sample limits.

Override process limits with a validated Map:

    let limits = Map::new()
    limits.set("timeout_ms", 60000)
    limits.set("max_stdout_bytes", 16777216)
    limits.set("max_stderr_bytes", 524288)

    let engine = process.process_backend(
        "/opt/ballistics/bin/ballistics",
        nil,
        limits
    )

Unknown options and non-positive or non-integer limits are rejected. Timeout,
spawn, and output-limit failures become `process_error` values. Valid engine
error envelopes retain their protocol code, field path or source line/column,
stderr diagnostics, and exit code. Malformed, truncated, contaminated,
non-finite, wrong-key, wrong-status, and exit-inconsistent responses fail closed
as `invalid_engine_response`.

A successful response must contain one to 10,000 samples, with exactly one
`terminal` flag on the final sample. Its terminal distance, time, speed, and
energy must agree with the summary using the solve-json fixture convention:
`abs(sample - summary) <= 1e-10 + 1e-9 * max(abs(sample), abs(summary))`.

`exec_argv` is not a sandbox: the configured executable runs with the Lattice
process's operating-system permissions and environment. The backend is
unsupported in browser/WASM builds because direct process execution is
unsupported there.

## Verification

From the repository root:

    make test-ballistics-lab

The target runs the unit/domain tests and executable reference experiment on
StackVM, tree-walk, and RegVM. Its backend test is hermetic but still launches a
portable fake engine as a real child, exercising argv, stdin, stdout, stderr,
exit status, bounds, and hostile response decoding.

To exercise an actual engine executable on all three Lattice backends:

    make test-ballistics-lab-engine \
        BALLISTICS_ENGINE=/absolute/path/to/ballistics \
        BALLISTICS_ENGINE_VERSION=0.24.1

`BALLISTICS_ENGINE_VERSION` is optional; omitting it disables the exact engine
version pin while retaining strict schema-v1 validation.

To emit the reference experiment as a solve-json v1 request:

    ./clat examples/ballistics_lab/reference_experiment.lat --json
