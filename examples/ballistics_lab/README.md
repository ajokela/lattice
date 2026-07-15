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
- reference_experiment.lat is an executable construction example.

The future process backend can serialize the Map returned by
experiment_to_v1_request, pass it to ballistics solve-json with exec_argv,
and decode the response without changing this domain API.

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

## Verification

From the repository root:

    make test-ballistics-lab

The target runs the unit/domain tests and executable reference experiment on
StackVM, tree-walk, and RegVM.

To emit the reference experiment as a solve-json v1 request:

    ./clat examples/ballistics_lab/reference_experiment.lat --json
