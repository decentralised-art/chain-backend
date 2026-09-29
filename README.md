# Decentralised Art server

[![Test (linux Debug)](https://github.com/hypermusic-ai/dcn-server/actions/workflows/test-linux-debug.yml/badge.svg)](https://github.com/hypermusic-ai/dcn-server/actions/workflows/test-linux-debug.yml) [![Test (Windows Debug)](https://github.com/hypermusic-ai/dcn-server/actions/workflows/test-windows-debug.yml/badge.svg)](https://github.com/hypermusic-ai/dcn-server/actions/workflows/test-windows-debug.yml) [![Test (macOS Debug)](https://github.com/hypermusic-ai/dcn-server/actions/workflows/test-macos-debug.yml/badge.svg)](https://github.com/hypermusic-ai/dcn-server/actions/workflows/test-macos-debug.yml)

---

[View docs](https://hypermusic-ai.github.io/dcn-server/) [![Docs](https://img.shields.io/badge/docs-online-blue)](https://hypermusic-ai.github.io/dcn-server/)

## Module docs

- [Events Pipeline](https://hypermusic-ai.github.io/dcn-server/readme_src_events.html)

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
cmake -S . -B build -DCMAKE_INSTALL_PREFIX=install -DDECENTRALIZED_ART_BUILD_TESTS=ON
```

- `-S .`: Specifies the source directory.
- `-B build`: Specifies the build directory.
- `-DCMAKE_INSTALL_PREFIX=install`: Specifies `<INSTALL_DIRECTORY>`.
- `-DDECENTRALIZED_ART_BUILD_TESTS=ON`: Enables tests.
- `-DDECENTRALIZED_ART_BUILD_STRESS_TESTS=ON`: Enables stress tests.
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

First start: deploy registry and runner (one time)

```sh
$env:DCN_CHAIN_RPC     = "https://eth-sepolia.g.alchemy.com/v2/<ALCHEMY_API_KEY>"
$env:DCN_BOOTSTRAP_KEY = "0x<deployer private key>"

.\<INSTALL_DIRECTORY>\bin\DecentralisedArtServer.exe `
  --chain-rpc-env DCN_CHAIN_RPC `
  --events-chain-id 11155111 `
  --publish-bootstrap `
  --publish --artifact-replicas "<path>" `
  --chain-local-source `
  --chain-batch-size 10
```

Next start

```sh
 .\<INSTALL_DIRECTORY>\bin\DecentralisedArtServer.exe --chain-rpc-env DCN_CHAIN_RPC --events-chain-id 11155111 `
>   --publish --artifact-replicas "<path>" --chain-local-source --chain-batch-size 10
```

---
📜 **License**:

👨‍💻 **Contributors**: [Sawyer](https://github.com/MisterSawyer)

💡 **Additional Notes**:
