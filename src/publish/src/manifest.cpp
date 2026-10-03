#include <format>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "chain.hpp"
#include "file.hpp"

#include "manifest.hpp"

namespace dcn::publish
{
    namespace
    {
        using json = nlohmann::json;

        constexpr std::size_t MAX_MANIFEST_BYTES = 64 * 1024;

        bool isAddress(const std::string & value)
        {
            const auto parsed = evmc::from_hex<chain::Address>(value);
            return parsed.has_value() && *parsed != chain::Address{};
        }

        std::string stringField(const json & value, const char * field)
        {
            const auto it = value.find(field);
            return it != value.end() && it->is_string() ? it->get<std::string>() : std::string{};
        }

        std::string hexField(const json & value, const char * field)
        {
            std::string text = stringField(value, field);
            return text.empty() ? text : chain::normalizeHex(std::move(text));
        }

        std::optional<std::uint64_t> unsignedField(const json & value, const char * field)
        {
            const auto it = value.find(field);
            if(it == value.end() || !it->is_number_unsigned()) return std::nullopt;
            return it->get<std::uint64_t>();
        }

        asio::awaitable<std::expected<void, std::string>> verifyProxy(
            const rpc::EthClient & client,
            const std::string_view label,
            const std::string & proxy,
            const std::string & expected_impl,
            const std::string & expected_code_hash,
            const std::string & expected_owner)
        {
            const auto implementation = co_await rpc::readProxyImplementation(client, proxy);
            if(!implementation)
            {
                co_return std::unexpected(std::format(
                    "{} implementation slot is unreadable: {}", label, implementation.error().message));
            }
            if(*implementation != expected_impl)
            {
                co_return std::unexpected(std::format(
                    "{} proxy points at an implementation the manifest does not pin", label));
            }

            const auto code_hash = co_await rpc::readCodeHash(client, expected_impl);
            if(!code_hash)
            {
                co_return std::unexpected(std::format(
                    "{} implementation code is unreadable: {}", label, code_hash.error().message));
            }
            if(!code_hash->has_value() || **code_hash != expected_code_hash)
            {
                co_return std::unexpected(std::format(
                    "{} implementation code hash does not match the manifest", label));
            }

            const auto owner = co_await rpc::readOwner(client, proxy);
            if(!owner)
            {
                co_return std::unexpected(std::format(
                    "{} upgrade owner is unreadable: {}", label, owner.error().message));
            }
            if(*owner != expected_owner)
            {
                co_return std::unexpected(std::format(
                    "{} upgrade owner {} is not the manifest owner {}", label, *owner, expected_owner));
            }

            co_return std::expected<void, std::string>{};
        }
    }

    std::expected<DeploymentManifest, std::string> readManifest(const std::filesystem::path & path)
    {
        std::error_code ec;
        if(!std::filesystem::is_regular_file(path, ec) || ec)
        {
            return std::unexpected("deployment manifest does not exist");
        }

        if(std::filesystem::file_size(path, ec) > MAX_MANIFEST_BYTES || ec)
        {
            return std::unexpected("deployment manifest is too large");
        }

        std::ifstream input(path, std::ios::binary);
        if(!input.is_open()) return std::unexpected("deployment manifest cannot be opened");

        std::ostringstream buffer;
        buffer << input.rdbuf();

        const json value = json::parse(buffer.str(), nullptr, false);
        if(value.is_discarded() || !value.is_object())
        {
            return std::unexpected("deployment manifest is not a JSON object");
        }

        const auto chain_id = unsignedField(value, "chain_id");
        const auto deployment_block = unsignedField(value, "deployment_block");
        const auto protocol_version = unsignedField(value, "protocol_version");
        if(!chain_id || *chain_id == 0) return std::unexpected("manifest chain_id is missing");
        if(!deployment_block
           || *deployment_block > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        {
            return std::unexpected("manifest deployment_block is invalid");
        }
        if(!protocol_version) return std::unexpected("manifest protocol_version is missing");

        DeploymentManifest manifest{
            .chain_id = *chain_id,
            .registry = hexField(value, "registry"),
            .runner = hexField(value, "runner"),
            .registry_impl = hexField(value, "registry_impl"),
            .runner_impl = hexField(value, "runner_impl"),
            .registry_impl_code_hash = hexField(value, "registry_impl_code_hash"),
            .runner_impl_code_hash = hexField(value, "runner_impl_code_hash"),
            .registry_tx = hexField(value, "registry_tx"),
            .runner_tx = hexField(value, "runner_tx"),
            .deployment_block = static_cast<std::int64_t>(*deployment_block),
            .protocol_id = hexField(value, "protocol_id"),
            .protocol_version = *protocol_version,
            .solc_version = stringField(value, "solc_version"),
            .pt_revision = stringField(value, "pt_revision"),
            .owner = hexField(value, "owner")
        };

        // A manifest without a runner leaves all three runner fields empty.
        const bool has_runner =
            !manifest.runner.empty() || !manifest.runner_impl.empty() || !manifest.runner_impl_code_hash.empty();

        std::vector<std::pair<const char *, const std::string *>> addresses{
            {"registry", &manifest.registry},
            {"registry_impl", &manifest.registry_impl},
            {"owner", &manifest.owner}};
        std::vector<std::pair<const char *, const std::string *>> hashes{
            {"registry_impl_code_hash", &manifest.registry_impl_code_hash},
            {"protocol_id", &manifest.protocol_id}};
        if(has_runner)
        {
            addresses.emplace_back("runner", &manifest.runner);
            addresses.emplace_back("runner_impl", &manifest.runner_impl);
            hashes.emplace_back("runner_impl_code_hash", &manifest.runner_impl_code_hash);
        }

        for(const auto & [field, address] : addresses)
        {
            if(!isAddress(*address))
            {
                return std::unexpected(std::string("manifest ") + field + " is not a non-zero address");
            }
        }
        for(const auto & [field, hash] : hashes)
        {
            if(!chain::isBytes32Hex(*hash))
            {
                return std::unexpected(std::string("manifest ") + field + " is not a 32-byte hash");
            }
        }

        return manifest;
    }

    std::string formatManifest(const DeploymentManifest & manifest)
    {
        const json value{
            {"chain_id", manifest.chain_id},
            {"registry", manifest.registry},
            {"runner", manifest.runner},
            {"registry_impl", manifest.registry_impl},
            {"runner_impl", manifest.runner_impl},
            {"registry_impl_code_hash", manifest.registry_impl_code_hash},
            {"runner_impl_code_hash", manifest.runner_impl_code_hash},
            {"registry_tx", manifest.registry_tx},
            {"runner_tx", manifest.runner_tx},
            {"deployment_block", static_cast<std::uint64_t>(manifest.deployment_block)},
            {"protocol_id", manifest.protocol_id},
            {"protocol_version", manifest.protocol_version},
            {"solc_version", manifest.solc_version},
            {"pt_revision", manifest.pt_revision},
            {"owner", manifest.owner}};
        return value.dump(2);
    }

    std::expected<void, std::string> writeManifest(
        const std::filesystem::path & path,
        const DeploymentManifest & manifest)
    {
        const auto written = file::writeFileDurably(path, formatManifest(manifest));
        if(!written)
        {
            return std::unexpected(std::format("manifest write failed: {}", written.error()));
        }
        return {};
    }

    std::string_view contractLabel(const PtContract contract)
    {
        return contract == PtContract::REGISTRY ? "registry" : "runner";
    }

    asio::awaitable<std::expected<void, std::string>> verifyContract(
        const rpc::EthClient & client,
        const DeploymentManifest & manifest,
        const PtContract contract)
    {
        if(contract == PtContract::RUNNER)
        {
            const auto runner = co_await rpc::verifyPtRunner(client, manifest.runner);
            if(!runner) co_return std::unexpected(runner.error());

            co_return co_await verifyProxy(client, "runner", manifest.runner, manifest.runner_impl,
                manifest.runner_impl_code_hash, manifest.owner);
        }

        const auto registry_identity = co_await rpc::verifyPtRegistry(client, manifest.chain_id, manifest.registry);
        if(!registry_identity) co_return std::unexpected(registry_identity.error());

        if(registry_identity->protocol_id != manifest.protocol_id
           || registry_identity->protocol_version != manifest.protocol_version)
        {
            co_return std::unexpected("deployed registry protocol identity does not match the manifest");
        }

        co_return co_await verifyProxy(client, "registry", manifest.registry, manifest.registry_impl,
            manifest.registry_impl_code_hash, manifest.owner);
    }

    asio::awaitable<std::expected<void, std::string>> verifyDeployment(
        const rpc::EthClient & client,
        const DeploymentManifest & manifest)
    {
        const auto registry = co_await verifyContract(client, manifest, PtContract::REGISTRY);
        if(!registry) co_return std::unexpected(registry.error());

        if(manifest.runner.empty()) co_return std::expected<void, std::string>{};
        co_return co_await verifyContract(client, manifest, PtContract::RUNNER);
    }
}
