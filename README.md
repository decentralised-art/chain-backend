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

## Sepolia On chain config

First start: deploy registry and runner, then keep serving. Every start that
passes a deploy or upgrade flag runs it again, so drop the flag afterwards.

```sh
$env:DCN_CHAIN_RPC    = "https://eth-sepolia.g.alchemy.com/v2/<ALCHEMY_API_KEY>"
$env:DCN_DEPLOYER_KEY = "0x<deployer private key>"

.\<INSTALL_DIRECTORY>\bin\DecentralisedArtServer.exe `
  --chain-rpc-env DCN_CHAIN_RPC `
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
.\<INSTALL_DIRECTORY>\bin\DecentralisedArtServer.exe --chain-rpc-env DCN_CHAIN_RPC --events-chain-id 11155111 `
  --publish-upgrade --publish

# or deploy a separate runner proxy; the manifest points at it from now on
# (--publish-deploy-registry does the same for the registry and keeps the runner)
.\<INSTALL_DIRECTORY>\bin\DecentralisedArtServer.exe --chain-rpc-env DCN_CHAIN_RPC --events-chain-id 11155111 `
  --publish-deploy-runner --publish
```

Next start

```sh
 .\<INSTALL_DIRECTORY>\bin\DecentralisedArtServer.exe --chain-rpc-env DCN_CHAIN_RPC --events-chain-id 11155111 `
>   --publish --chain-local-source --chain-batch-size 10
```

---
📜 **License**:

👨‍💻 **Contributors**: [Sawyer](https://github.com/MisterSawyer)

💡 **Additional Notes**:
