#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "address.hpp"

#include "eth_tx.hpp"

struct secp256k1_context_struct;

namespace dcn::publish
{
    /// Holds the operator key used to bootstrap a PT deployment and is the only
    /// component that can sign with it. The key never leaves this object: it is not
    /// logged, not copied into configuration and not persisted anywhere else, and the
    /// buffers it passed through while loading are wiped.
    class Signer
    {
        public:
            /// Loads a 32-byte secp256k1 secret from a key file containing a single hex
            /// value. On POSIX the file must not be accessible to group or others. A
            /// command-line value is deliberately not accepted: it would appear in the
            /// process table and in shell history.
            static std::expected<std::shared_ptr<Signer>, std::string> fromKeyFile(
                const std::filesystem::path & key_path);

            /// Loads the same hex form from an environment variable, then removes the
            /// variable so child processes do not inherit it.
            static std::expected<std::shared_ptr<Signer>, std::string> fromEnvironment(
                const std::string & variable_name);

            Signer(const Signer &) = delete;
            Signer & operator=(const Signer &) = delete;

            ~Signer();

            const chain::Address & address() const;

            /// 65-byte `r || s || v` with v in {27, 28}. libsecp256k1 emits only
            /// canonical low-`s` signatures.
            std::expected<std::vector<std::uint8_t>, std::string> signDigest(
                const evmc::bytes32 & digest) const;

            std::expected<std::string, std::string> signTransaction(const Transaction & transaction) const;

        private:
            Signer(std::array<std::uint8_t, 32> secret, chain::Address address, secp256k1_context_struct * context);

            static std::expected<std::shared_ptr<Signer>, std::string> _fromHex(std::string & secret_text);

            std::array<std::uint8_t, 32> _secret{};
            chain::Address _address{};
            secp256k1_context_struct * _context = nullptr;
    };
}
