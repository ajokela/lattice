# Lattice Package Registry

A self-hosted package registry server for the Lattice language, written entirely in Lattice.

## Quick Start

### Start the registry server

```sh
# Default port 8080
clat registry/server.lat

# Custom port
clat registry/server.lat 3000
```

### Install packages from the registry

```sh
# Point the Lattice client at your local registry
export LATTICE_REGISTRY=http://localhost:8080/v1

# Create a project and add dependencies
clat init
clat add hello
clat add math-extra
clat install
```

### Publish a package

```sh
# Publish a package directory containing lattice.toml and main.lat
clat registry/publish.lat registry/packages/hello/1.0.0

# Publish to a specific registry
clat registry/publish.lat my-package/ http://my-registry:3000/v1
```

## API Endpoints

| Method | Path | Description |
|--------|------|-------------|
| `GET` | `/v1/health` | Health check |
| `GET` | `/v1/packages/<name>/versions` | List available versions |
| `GET` | `/v1/packages/<name>/<version>` | Download package source |
| `POST` | `/v1/packages/<name>/<version>` | Publish a new version |

### List versions

```sh
curl http://localhost:8080/v1/packages/hello/versions
# {"versions":["1.0.0"]}
```

### Download a package

```sh
curl http://localhost:8080/v1/packages/hello/1.0.0
# Returns the main.lat source code
```

### Publish a package

```sh
curl -X POST http://localhost:8080/v1/packages/mylib/1.0.0 \
  -H "Content-Type: application/json" \
  -d '{"source":"fn greet() { return \"hi\" }","toml":"[package]\nname = \"mylib\"\nversion = \"1.0.0\"\n"}'
# {"status":"published","package":"mylib","version":"1.0.0"}
```

## Directory Structure

```
registry/
  server.lat              # Registry HTTP server
  publish.lat             # CLI publish tool
  README.md               # This file
  packages/               # Package storage
    <name>/
      versions.json       # {"versions": ["1.0.0", "1.1.0"]}
      <version>/
        lattice.toml      # Package metadata
        main.lat          # Package source
```

## Example Packages

Three example packages are included for testing:

- **hello** (1.0.0) - Greeting library with i18n support
- **math-extra** (1.0.0) - Factorial, fibonacci, gcd, prime checking, etc.
- **string-utils** (1.0.0) - String repeat, truncate, slug, word count, etc.

## Environment Variables

- `LATTICE_REGISTRY` - Registry URL for the `clat` client (e.g., `http://localhost:8080/v1`)
