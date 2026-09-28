#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <random>

#include <secp256k1.h>
#include <secp256k1_recovery.h>

#include "chain.hpp"

#include "signer.hpp"

namespace dcn::publish
{
    namespace
    {
        // Writes through a volatile view so the wipe is not optimized away.
        void secureZero(void * data, const std::size_t size)
        {
            volatile std::uint8_t * bytes = static_cast<volatile std::uint8_t *>(data);
            for(std::size_t i = 0; i < size; ++i) bytes[i] = 0;
        }

        void secureZero(std::string & text)
        {
            secureZero(text.data(), text.size());
            text.clear();
        }

        int hexNibble(const char c)
        {
            if(c >= '0' && c <= '9') return c - '0';
            if(c >= 'a' && c <= 'f') return c - 'a' + 10;
            if(c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        }

        // Decodes straight into the fixed buffer so no intermediate copy of the secret
        // is left behind.
        std::expected<std::array<std::uint8_t, 32>, std::string> parseSecret(const std::string & text)
        {
            std::size_t begin = 0;
            std::size_t end = text.size();
            while(begin < end && std::isspace(static_cast<unsigned char>(text[begin])) != 0) ++begin;
            while(end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) --end;
            if(end - begin >= 2 && text[begin] == '0' && (text[begin + 1] == 'x' || text[begin + 1] == 'X'))
            {
                begin += 2;
            }

            if(end - begin != 64)
            {
                return std::unexpected("signing key must be 32 hex-encoded bytes");
            }

            std::array<std::uint8_t, 32> secret{};
            for(std::size_t i = 0; i < secret.size(); ++i)
            {
                const int high = hexNibble(text[begin + 2 * i]);
                const int low = hexNibble(text[begin + 2 * i + 1]);
                if(high < 0 || low < 0)
                {
                    secureZero(secret.data(), secret.size());
                    return std::unexpected("signing key is not valid hex");
                }
                secret[i] = static_cast<std::uint8_t>((high << 4) | low);
            }
            return secret;
        }

        secp256k1_context * createRandomizedContext()
        {
            secp256k1_context * context = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
            if(context == nullptr) return nullptr;

            // Blinding against side channels on the signing path.
            std::array<unsigned char, 32> seed{};
            std::random_device entropy;
            for(std::size_t i = 0; i < seed.size(); i += 4)
            {
                const auto value = entropy();
                for(std::size_t b = 0; b < 4; ++b)
                {
                    seed[i + b] = static_cast<unsigned char>((value >> (8 * b)) & 0xFFu);
                }
            }

            const int randomized = secp256k1_context_randomize(context, seed.data());
            secureZero(seed.data(), seed.size());
            if(randomized != 1)
            {
                secp256k1_context_destroy(context);
                return nullptr;
            }
            return context;
        }

#if !defined(_WIN32)
        bool isPrivateToOwner(const std::filesystem::path & path)
        {
            std::error_code ec;
            const auto permissions = std::filesystem::status(path, ec).permissions();
            if(ec) return false;
            constexpr auto shared = std::filesystem::perms::group_all | std::filesystem::perms::others_all;
            return (permissions & shared) == std::filesystem::perms::none;
        }
#endif
    }

    Signer::Signer(std::array<std::uint8_t, 32> secret, chain::Address address, secp256k1_context_struct * context)
        : _secret(secret)
        , _address(address)
        , _context(context)
    {
        secureZero(secret.data(), secret.size());
    }

    Signer::~Signer()
    {
        secureZero(_secret.data(), _secret.size());
        if(_context != nullptr) secp256k1_context_destroy(_context);
    }

    std::expected<std::shared_ptr<Signer>, std::string> Signer::_fromHex(std::string & secret_text)
    {
        auto secret = parseSecret(secret_text);
        secureZero(secret_text);
        if(!secret) return std::unexpected(secret.error());

        secp256k1_context * context = createRandomizedContext();
        if(context == nullptr)
        {
            secureZero(secret->data(), secret->size());
            return std::unexpected("failed to create secp256k1 context");
        }

        secp256k1_pubkey pubkey;
        if(!secp256k1_ec_seckey_verify(context, secret->data()) || !secp256k1_ec_pubkey_create(context, &pubkey, secret->data()))
        {
            secureZero(secret->data(), secret->size());
            secp256k1_context_destroy(context);
            return std::unexpected("signing key is not a valid secp256k1 secret");
        }

        std::array<std::uint8_t, 65> serialized{};
        std::size_t length = serialized.size();
        secp256k1_ec_pubkey_serialize(context, serialized.data(), &length, &pubkey, SECP256K1_EC_UNCOMPRESSED);
        const chain::Address address = chain::ethAddressFromPublicKey(serialized.data(), length);

        std::shared_ptr<Signer> signer(new Signer(*secret, address, context));
        secureZero(secret->data(), secret->size());
        return signer;
    }

    std::expected<std::shared_ptr<Signer>, std::string> Signer::fromKeyFile(
        const std::filesystem::path & key_path)
    {
        std::error_code ec;
        if(!std::filesystem::is_regular_file(key_path, ec) || ec)
        {
            return std::unexpected("signing key file does not exist");
        }

#if !defined(_WIN32)
        if(!isPrivateToOwner(key_path))
        {
            return std::unexpected("signing key file is readable by group or others; restrict it to its owner");
        }
#endif

        std::ifstream input(key_path, std::ios::binary);
        if(!input.is_open()) return std::unexpected("signing key file cannot be opened");

        std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        if(input.bad())
        {
            secureZero(text);
            return std::unexpected("signing key file cannot be read");
        }

        return _fromHex(text);
    }

    std::expected<std::shared_ptr<Signer>, std::string> Signer::fromEnvironment(
        const std::string & variable_name)
    {
        const char * value = std::getenv(variable_name.c_str());
        if(value == nullptr || *value == '\0')
        {
            return std::unexpected("signing key environment variable is not set");
        }

        std::string text(value);
#if defined(_WIN32)
        (void)::_putenv_s(variable_name.c_str(), "");
#else
        (void)::unsetenv(variable_name.c_str());
#endif
        return _fromHex(text);
    }

    const chain::Address & Signer::address() const
    {
        return _address;
    }

    std::expected<std::vector<std::uint8_t>, std::string> Signer::signDigest(
        const evmc::bytes32 & digest) const
    {
        secp256k1_ecdsa_recoverable_signature signature;
        if(!secp256k1_ecdsa_sign_recoverable(_context, &signature, digest.bytes, _secret.data(), nullptr, nullptr))
        {
            return std::unexpected("signing failed");
        }

        std::vector<std::uint8_t> out(65, 0);
        int recovery_id = 0;
        if(!secp256k1_ecdsa_recoverable_signature_serialize_compact(_context, out.data(), &recovery_id, &signature))
        {
            return std::unexpected("signing failed");
        }
        out[64] = static_cast<std::uint8_t>(27 + recovery_id);
        return out;
    }

    std::expected<std::string, std::string> Signer::signTransaction(const Transaction & transaction) const
    {
        const auto signature = signDigest(signingHash(transaction));
        if(!signature) return std::unexpected(signature.error());

        std::string raw = encodeSignedTransaction(transaction, *signature);
        if(raw.empty()) return std::unexpected("failed to encode signed transaction");

        return raw;
    }
}
