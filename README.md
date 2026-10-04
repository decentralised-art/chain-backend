# chain-backend

C++ chain API for decentralised.art.

[![Test (linux Debug)](https://github.com/decentralised-art/chain-backend/actions/workflows/test-linux-debug.yml/badge.svg)](https://github.com/decentralised-art/chain-backend/actions/workflows/test-linux-debug.yml) [![Test (Windows Debug)](https://github.com/decentralised-art/chain-backend/actions/workflows/test-windows-debug.yml/badge.svg)](https://github.com/decentralised-art/chain-backend/actions/workflows/test-windows-debug.yml) [![Test (macOS Debug)](https://github.com/decentralised-art/chain-backend/actions/workflows/test-macos-debug.yml/badge.svg)](https://github.com/decentralised-art/chain-backend/actions/workflows/test-macos-debug.yml)

---

[Public API documentation](https://decentralised.art/documentation/)

## Module docs

- [Events Pipeline](src/events/README.md)

## Dependencies

This project requires the following libraries:

### Installed on the machine

- [**OpenSSL**](https://www.openssl.org/)
- [**npm**](https://www.npmjs.com/)

### Fetched automatically

- [**Asio**](https://think-async.com/Asio/)
- [**spdlog**](https://github.com/gabime/spdlog)
- [**cURL**](https://curl.se/)
- [**abseil**](https://github.com/abseil/abseil-cpp)
- [**Protobuf**](https://protobuf.dev/)
- [**jwt-cpp**](https://github.com/Thalhammer/jwt-cpp)
- [**secp256k1**](https://github.com/bitcoin-core/secp256k1)
- [**solc**](https://github.com/ethereum/solidity)
- [**evmc**](https://github.com/ethereum/evmc)
- [**evmone**](https://github.com/ethereum/evmone)

#### Testing

- [**GTest**](https://github.com/google/googletest)

---

## Configuring

To configure the project using CMake, run the following command:

```sh
cmake -S . -B build -DCMAKE_INSTALL_PREFIX=install -DDECENTRALISED_ART_BUILD_TESTS=ON
```

- `-S .`: Specifies the source directory.
- `-B build`: Specifies the build directory.
- `-DCMAKE_INSTALL_PREFIX=install`: Specifies `<INSTALL_DIRECTORY>`.
- `-DDECENTRALISED_ART_BUILD_TESTS=ON`: Enables tests.
- `-DDECENTRALISED_ART_BUILD_STRESS_TESTS=ON`: Enables stress tests.
- `-DDECENTRALISED_ART_USE_SUBMODULE_PT=ON`: Enables use of local PT submodule.

---

## Building

|type|command|
|---|---|
|Debug|`cmake --build build --config Debug`|
|Release|`cmake --build build --config Release`|
|RelWithDebInfo|`cmake --build build --config RelWithDebInfo`|

## Installing

|type|command|
|---|---|
|Debug|`cmake --build build --config Debug --target install`|
|Release|`cmake --build build --config Release --target install`|
|RelWithDebInfo|`cmake --build build --config RelWithDebInfo --target install`|

---

## Start the server

```sh
<INSTALL_DIRECTORY>/bin/DecentralisedArtServer.exe
```

### Command-line arguments

Pass values as separate arguments, for example `--port 54321`; `--option=value`
syntax is not supported. Flags without a value are disabled unless supplied.
Quote paths that contain spaces. `<storage>` below is
`<INSTALL_DIRECTORY>/storage`, derived from the executable's directory.

| Argument | Default | Description |
| --- | --- | --- |
| `-h`, `--help` | Off | Print the command-line help and exit. |
| `--version` | Off | Print the version and build timestamp, then exit. |
| `--verbose` | Off | Set the verbose configuration flag. Currently this does not change logger levels: console logging is at info and file logging is at debug. |
| `--port <number>` | `54321` | TCP port for the HTTP server. |
| `--max-request-body <bytes>` | `1048576` (1 MiB) | Maximum HTTP request body size; must be positive. Larger requests receive HTTP `413`. |
| `--auth-origin <origins>` | `http://localhost:<port>` | Comma-separated `http(s)://host[:port]` origins of the sites users sign in through (stored as a browser reports them: lower case, default port dropped), for example `https://decentralised.art,https://api.decentralised.art`. The EIP-4361 sign-in message from `/nonce` names the `origin` query parameter (which must be listed), else the request's `Origin` header when it is listed, else the first entry, so wallets warn users signing it on any other site. |
| `--chain-rpc <url>` | Unset | HTTP or HTTPS Ethereum JSON-RPC endpoint for event sync, execution and publication. Mutually exclusive with `--chain-rpc-env`. |
| `--chain-rpc-env <name>` | Unset | Read the RPC URL from a nonempty environment variable, keeping provider credentials out of the process arguments. Mutually exclusive with `--chain-rpc`. |
| `--chain-registry <address>` | From manifest, if present | PT registry proxy address. Required for RPC access without a manifest; must match the registry if a manifest exists. |
| `--chain-runner <address>` | From manifest, if present | PT runner proxy address for executing chain connectors. Must match the runner if a manifest exists. |
| `--chain-start-block <number>` | Manifest deployment block | Starting block for event sync when no local cursor exists. Required for RPC access without a manifest. |
| `--chain-poll-ms <milliseconds>` | `60000` | Interval between chain event polls. |
| `--chain-confirmations <blocks>` | `12` | Finality depth used when the RPC endpoint does not support `safe` and `finalized` block tags. |
| `--chain-batch-size <blocks>` | `500` | Maximum blocks fetched per `eth_getLogs` request. |
| `--chain-local-source` | Off | Ingest events from the in-process EVM. Can be used on its own or alongside an RPC source. |
| `--execute-max-gas <gas>` | `100000000` | Gas cap for the runner `eth_call` used by `POST /execute` for chain connectors. |
| `--execute-block-tag <tag>` | `safe` | Chain state used by `POST /execute`: `safe`, `finalized` or `latest`. |
| `--registry-db <path>` | `<storage>/registry.sqlite` | SQLite registry database path. Use `:memory:` for an in-memory registry. |
| `--registry-wal-sync-ms <milliseconds>` | `30000` | Interval between passive SQLite WAL checkpoints for a file-backed registry database. |
| `--events-db <path>` | `<storage>/events/events_hot.sqlite` | SQLite database for hot event storage. |
| `--feed-db <path>` | `<storage>/feed/feed.sqlite` | SQLite database for the projected feed. |
| `--feed-archive-root <path>` | `<storage>/feed/archive` | Directory for feed archive segments. |
| `--events-chain-id <id>` | `1` | Positive decimal 64-bit chain ID for event ingestion and feed projection. Must match the RPC endpoint and deployment manifest when used. |
| `--events-hot-window-days <days>` | `90` | Retention window for hot event storage. |
| `--events-projector-ms <milliseconds>` | `200` | Interval between projector passes when idle. |
| `--events-archive-ms <milliseconds>` | `30000` | Interval between archive maintenance passes. |
| `--events-reorg-window-blocks <blocks>` | `2048` | Rolling block window retained for reorganization reconciliation. |
| `--events-outbox-retention-days <days>` | `7` | Retention window for replay outbox rows. |
| `--events-projector-retry-attempts <count>` | `5` | Consecutive failed projector passes on a row before it is dead-lettered. |
| `--events-dead-letter-sweep-ms <milliseconds>` | `60000` | Interval between idle-time retries of dead-lettered rows. |
| `--publish` | Off | Enable `/publish/*` routes for owner-paid publication. Requires an RPC endpoint; does not require a deployer key. |
| `--deployment-manifest <path>` | `<storage>/deployment.json` | PT deployment manifest supplying contract addresses and the deployment block. An existing unreadable or invalid manifest stops startup. Deployment actions write to this path. |
| `--publish-deploy` | Off | Deploy new registry and runner proxies with the deployer key, update the manifest, then start serving. |
| `--publish-deploy-registry` | Off | Deploy a new registry proxy and update or create the manifest, keeping any manifest runner. |
| `--publish-deploy-runner` | Off | Deploy a new runner proxy and update the manifest. Requires a manifest registry. |
| `--publish-upgrade` | Off | Upgrade both manifest proxies to new implementations, update the manifest, then start serving. Proxy addresses are preserved. |
| `--publish-upgrade-registry` | Off | Upgrade the manifest registry proxy to a new implementation and update the manifest. |
| `--publish-upgrade-runner` | Off | Upgrade the manifest runner proxy to a new implementation and update the manifest. |
| `--deployer-key-file <path>` | Unset | File containing a 32-byte hex deployer private key, optionally prefixed with `0x`. Takes precedence over `--deployer-key-env`; on non-Windows systems, group and other permissions must be disabled. |
| `--deployer-key-env <name>` | Unset | Explicitly name the environment variable containing the hex deployer private key, used when no key file is supplied. |
| `--deployer-max-gas <gas>` | `12000000` | Maximum gas the deployer will sign for one transaction. An estimate above this limit fails the action. |
| `--deployer-max-fee-gwei <gwei>` | `200` | Maximum total fee per gas the deployer will sign, in whole gwei. A required fee above this limit fails the action. |

Deploy and upgrade flags require an RPC endpoint and a deployer key supplied via
`--deployer-key-file` or `--deployer-key-env`. They run on every startup that
includes them, registry before runner, and rewrite the manifest after each action.
Deploying and upgrading the same contract in one run is rejected. Remove these
flags after the intended deployment or upgrade.

## Sepolia On chain config

First start: deploy registry and runner, then keep serving. Every start that
passes a deploy or upgrade flag runs it again, so drop the flag afterwards.

```sh
$env:DECENTRALISED_ART_CHAIN_RPC = "https://eth-sepolia.g.alchemy.com/v2/<ALCHEMY_API_KEY>"
$env:SEPOLIA_DEPLOYER_KEY = "0x<deployer private key>"

.\<INSTALL_DIRECTORY>\bin\DecentralisedArtServer.exe `
  --chain-rpc-env DECENTRALISED_ART_CHAIN_RPC `
  --deployer-key-env SEPOLIA_DEPLOYER_KEY `
  --events-chain-id 11155111 `
  --publish-deploy `
  --publish `
  --chain-local-source `
  --chain-batch-size 10
```

Upgrade or replace contracts of the manifest deployment, then keep serving

```sh
# keep the proxy addresses, swap their implementations
# (--publish-upgrade-registry / --publish-upgrade-runner for just one)
.\<INSTALL_DIRECTORY>\bin\DecentralisedArtServer.exe --chain-rpc-env DECENTRALISED_ART_CHAIN_RPC --events-chain-id 11155111 `
  --deployer-key-env SEPOLIA_DEPLOYER_KEY --publish-upgrade --publish

# or deploy a separate runner proxy; the manifest points at it from now on
# (--publish-deploy-registry does the same for the registry and keeps the runner)
.\<INSTALL_DIRECTORY>\bin\DecentralisedArtServer.exe --chain-rpc-env DECENTRALISED_ART_CHAIN_RPC --events-chain-id 11155111 `
  --deployer-key-env SEPOLIA_DEPLOYER_KEY --publish-deploy-runner --publish
```

Next start

```sh
 .\<INSTALL_DIRECTORY>\bin\DecentralisedArtServer.exe --chain-rpc-env DECENTRALISED_ART_CHAIN_RPC --events-chain-id 11155111 `
>   --publish --chain-local-source --chain-batch-size 10
```

---
📜 **License**:

👨‍💻 **Contributors**: [Sawyer](https://github.com/MisterSawyer)

💡 **Additional Notes**:
