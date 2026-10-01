# AGENTS.md

## Scope

Instructions in this file apply to the repository root (`chain-backend`) and all subdirectories.

## Repository Structure

Key directories in this repository:

- `src/` - C++ implementation of the decentralised.art chain backend
- `include/` - public/internal header files
- `tests/` - unit/integration tests
- `cmake/` - CMake helper modules
- `resources/` - runtime resources and supporting data
- `submodule/pt/` - PT protocol Solidity repository (git submodule)

## Multi-Repository Context

This workspace contains two primary repositories with different ownership:

- `submodule/pt` is the Solidity PT protocol and is the **source of truth** for protocol behavior.
- `chain-backend` (this repository) is a C++ REST API server that interacts with the PT protocol.

When protocol behavior is unclear, agents should treat `submodule/pt` as authoritative and ensure server-side behavior aligns with it.

## Build Directory Policy

Agents must use a build directory for all work:

- `build-agent` for CMake build files
- `install-agent` for install output (`-DCMAKE_INSTALL_PREFIX=install-agent`)

## Build/Test Flags

When building tests, always configure CMake with these options:

- `-DDECENTRALISED_ART_BUILD_TESTS=ON`
- `-DDECENTRALISED_ART_USE_SUBMODULE_PT=ON`

Use build configuration Debug or RelWithDebInfo.

The unit test build target is:

- `DecentralisedArtServerTests`

## Test Rebuild Policy

Do **not** rebuild tests by default for every change.

## Build/Test Execution Gate

Do **not** run build or test commands automatically after making changes.

Only run build/test commands when the user gives an explicit command/request to do so.

Examples of gated commands include:

- `cmake --build ...`
- `ctest ...`
- running `DecentralisedArtServerTests` directly

Rebuild `DecentralisedArtServerTests` when at least one of these is true:

- Any file under `src/`, `include/`, or `tests/` changes (`.cpp`, `.hpp`, etc.)
- CMake/test wiring changes (`CMakeLists.txt`, `tests/CMakeLists.txt`, `cmake/*.cmake`)
- Generated/build inputs for tests changed (protobufs, linked dependency config, compile options)
- `submodule/pt` changes affect server behavior or test expectations

Skip test rebuild when changes are non-compiled only, for example:

- Documentation (`README.md`, `AGENTS.md`, docs)
- Static resources/UI assets only (`resources/html`, `resources/js`, `resources/styles`, images), unless tests explicitly depend on them
- Pure formatting/comment-only edits in files that do not alter compiled/tested behavior

When rebuild is needed, prefer incremental verification:

- Build `DecentralisedArtServerTests`
- Run only relevant test cases first (targeted filter)
- Run broader/full test pass only when the scope of changes justifies it

## Temporary working area

All documentation, plans, tasks descriptions, etc. should be placed in the `.agent/docs` directory.
All temporary files, logs, etc. should be placed in the `.agent/tmp` directory.
All temporary scripts, agents, etc. should be placed in the `.agent/bin` directory.

## Comments

When writing or modifying code, never add comments that describe old behavior, previous implementations, migration history, or what changed. Comments must only explain the current behavior, current intent, or current constraints.

Avoid comments like “previously…”, “old behavior…”, “changed from…”, “implemented in phase 2”, “temporary migration”, or anything tied to development history. The codebase should read as if the current implementation is the only relevant truth.

## Tests

All code should be covered by unit tests
