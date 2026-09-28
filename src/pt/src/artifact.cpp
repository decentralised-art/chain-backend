#include "artifact.hpp"

#include <algorithm>
#include <format>
#include <fstream>
#include <ranges>
#include <string_view>
#include <tuple>
#include <utility>

#include "chain.hpp"
#include "file.hpp"

namespace dcn::pt
{
    namespace
    {
        constexpr int ARTIFACT_SCHEMA_VERSION = 1;

        bool validHash(const std::string & value)
        {
            if(value.size() != 66 || value[0] != '0' || value[1] != 'x') return false;
            return std::ranges::all_of(value.substr(2), [](const unsigned char c)
            {
                return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
            });
        }

        std::filesystem::path artifactPath(
            const std::filesystem::path & root,
            const std::string & content_hash)
        {
            return root / (content_hash + ".json");
        }

        std::expected<std::string, std::string> readFile(const std::filesystem::path & path)
        {
            std::error_code ec;
            const auto size = std::filesystem::file_size(path, ec);
            if(ec) return std::unexpected("artifact is missing");
            if(size == 0 || size > MAX_ARTIFACT_BYTES)
            {
                return std::unexpected("artifact size is invalid");
            }

            std::ifstream input(path, std::ios::binary);
            if(!input) return std::unexpected("artifact cannot be opened");
            std::string bytes(static_cast<std::size_t>(size), '\0');
            if(!input.read(bytes.data(), static_cast<std::streamsize>(bytes.size())))
            {
                return std::unexpected("artifact cannot be read");
            }
            return bytes;
        }

        std::expected<void, std::string> writeVerified(
            const std::filesystem::path & root,
            const std::string & content_hash,
            const std::string & bytes)
        {
            const auto target = artifactPath(root, content_hash);
            if(const auto written = file::writeFileDurably(target, bytes); !written)
            {
                return std::unexpected(std::format("artifact cannot be written: {}", written.error()));
            }

            const auto stored = readFile(target);
            if(!stored || artifactContentHash(*stored) != content_hash)
            {
                return std::unexpected("artifact read-back hash mismatch");
            }
            return {};
        }

        std::expected<ArtifactBundle, std::string> parseBundleJson(const nlohmann::json & value)
        {
            try
            {
                if(!value.is_object() || value.size() != 10
                   || value.at("schema_version").get<int>() != ARTIFACT_SCHEMA_VERSION)
                {
                    return std::unexpected("unsupported artifact schema");
                }

                ArtifactBundle bundle;
                const auto kind = parseEntityKind(value.at("entity_type").get<std::string>());
                if(!kind) return std::unexpected("invalid artifact entity type");
                bundle.kind = *kind;
                bundle.name = value.at("name").get<std::string>();
                bundle.definition = value.at("definition");
                bundle.sol_src = value.at("sol_src").get<std::string>();
                bundle.generated_solidity = value.at("generated_solidity").get<std::string>();

                const auto & compiler = value.at("compiler");
                if(!bundle.definition.is_object() || !compiler.is_object() || compiler.size() != 7
                   || !compiler.at("imports").is_array())
                {
                    return std::unexpected("artifact structure is invalid");
                }
                bundle.pt_revision = compiler.at("pt_revision").get<std::string>();
                bundle.solc_version = compiler.at("solc_version").get<std::string>();
                bundle.evm_version = compiler.at("evm_version").get<std::string>();
                bundle.optimizer = compiler.at("optimizer").get<bool>();
                bundle.optimizer_runs = compiler.at("optimizer_runs").get<std::uint32_t>();
                bundle.via_ir = compiler.at("via_ir").get<bool>();
                bundle.imports = compiler.at("imports").get<std::vector<std::string>>();

                const auto & bytecode = value.at("bytecode");
                if(!bytecode.is_object() || bytecode.size() != 2 || !value.at("dependencies").is_array())
                {
                    return std::unexpected("artifact structure is invalid");
                }
                bundle.creation_code_hash = bytecode.at("creation_hash").get<std::string>();
                bundle.runtime_code_hash = bytecode.at("runtime_hash").get<std::string>();
                bundle.metadata_hash = value.at("metadata_hash").get<std::string>();

                for(const auto & dependency : value.at("dependencies"))
                {
                    if(!dependency.is_object() || dependency.size() != 3)
                    {
                        return std::unexpected("artifact dependency structure is invalid");
                    }
                    const auto dependency_kind = parseEntityKind(dependency.at("entity_type").get<std::string>());
                    if(!dependency_kind) return std::unexpected("invalid artifact dependency type");
                    bundle.dependencies.push_back(ArtifactDependency{
                        .kind = *dependency_kind,
                        .name = dependency.at("name").get<std::string>(),
                        .content_hash = dependency.at("content_hash").get<std::string>()});
                }

                if(!isValidEntityName(bundle.name) || bundle.generated_solidity.empty() || !validHash(bundle.pt_revision)
                   || bundle.solc_version.empty() || bundle.evm_version.empty()
                   || !validHash(bundle.creation_code_hash) || !validHash(bundle.runtime_code_hash)
                   || !validHash(bundle.metadata_hash)
                   || bundle.metadata_hash != chain::bytes32ToHex(computeMetadataHash(bundle.kind, bundle.name)))
                {
                    return std::unexpected("artifact is missing required values");
                }
                if(bundle.kind == EntityKind::CONNECTOR && !bundle.sol_src.empty())
                {
                    return std::unexpected("connector artifact has source fragment");
                }
                if(bundle.kind != EntityKind::CONNECTOR && bundle.sol_src.empty())
                {
                    return std::unexpected("artifact source fragment is missing");
                }
                if(std::ranges::any_of(bundle.dependencies, [](const ArtifactDependency & dependency)
                {
                    return !isValidEntityName(dependency.name) || !validHash(dependency.content_hash);
                }))
                {
                    return std::unexpected("artifact dependency is invalid");
                }
                if(!std::ranges::is_sorted(bundle.imports)
                   || std::ranges::adjacent_find(bundle.imports) != bundle.imports.end()
                   || !std::ranges::is_sorted(bundle.dependencies, {}, [](const ArtifactDependency & dependency)
                   {
                       return std::tuple{dependency.kind, dependency.name, dependency.content_hash};
                   })
                   || std::ranges::adjacent_find(
                       bundle.dependencies,
                       [](const ArtifactDependency & lhs, const ArtifactDependency & rhs)
                       {
                           return lhs.kind == rhs.kind && lhs.name == rhs.name;
                       }) != bundle.dependencies.end())
                {
                    return std::unexpected("artifact arrays are not canonical");
                }
                return bundle;
            }
            catch(const std::exception & error)
            {
                return std::unexpected(std::string("invalid artifact: ") + error.what());
            }
        }
    }

    std::string_view artifactStateName(const ArtifactState state)
    {
        switch(state)
        {
        case ArtifactState::AVAILABLE: return "available";
        case ArtifactState::MISSING: return "missing";
        case ArtifactState::INVALID: return "invalid";
        }
        return "invalid";
    }

    std::expected<std::string, std::string> encodeArtifactBundle(const ArtifactBundle & bundle)
    {
        ArtifactBundle canonical = bundle;
        std::ranges::sort(canonical.imports);
        canonical.imports.erase(
            std::ranges::unique(canonical.imports).begin(), canonical.imports.end());
        std::ranges::sort(canonical.dependencies, {}, [](const ArtifactDependency & dependency)
        {
            return std::tuple{dependency.kind, dependency.name, dependency.content_hash};
        });

        nlohmann::json dependencies = nlohmann::json::array();
        for(const auto & dependency : canonical.dependencies)
        {
            dependencies.push_back({
                {"content_hash", dependency.content_hash},
                {"entity_type", std::string(entityKindName(dependency.kind))},
                {"name", dependency.name}});
        }

        nlohmann::json value{
            {"bytecode", {
                {"creation_hash", bundle.creation_code_hash},
                {"runtime_hash", bundle.runtime_code_hash}}},
            {"compiler", {
                {"evm_version", bundle.evm_version},
                {"imports", canonical.imports},
                {"optimizer", canonical.optimizer},
                {"optimizer_runs", canonical.optimizer_runs},
                {"pt_revision", canonical.pt_revision},
                {"solc_version", canonical.solc_version},
                {"via_ir", canonical.via_ir}}},
            {"definition", canonical.definition},
            {"dependencies", std::move(dependencies)},
            {"entity_type", std::string(entityKindName(canonical.kind))},
            {"generated_solidity", canonical.generated_solidity},
            {"metadata_hash", canonical.metadata_hash},
            {"name", canonical.name},
            {"schema_version", ARTIFACT_SCHEMA_VERSION},
            {"sol_src", canonical.sol_src}};

        const auto validated = parseBundleJson(value);
        if(!validated) return std::unexpected(validated.error());
        try
        {
            const std::string bytes = value.dump();
            if(bytes.size() > MAX_ARTIFACT_BYTES) return std::unexpected("artifact exceeds size limit");
            return bytes;
        }
        catch(const std::exception & error)
        {
            return std::unexpected(std::string("artifact is not valid UTF-8 JSON: ") + error.what());
        }
    }

    std::expected<ArtifactBundle, std::string> decodeArtifactBundle(
        const std::string & bytes,
        const std::string & expected_content_hash)
    {
        if(bytes.empty() || bytes.size() > MAX_ARTIFACT_BYTES)
        {
            return std::unexpected("artifact size is invalid");
        }
        if(!expected_content_hash.empty()
           && (!validHash(expected_content_hash) || artifactContentHash(bytes) != expected_content_hash))
        {
            return std::unexpected("artifact content hash mismatch");
        }

        try
        {
            const nlohmann::json value = nlohmann::json::parse(bytes);
            if(value.dump() != bytes) return std::unexpected("artifact bytes are not canonical JSON");
            return parseBundleJson(value);
        }
        catch(const std::exception & error)
        {
            return std::unexpected(std::string("invalid artifact JSON: ") + error.what());
        }
    }

    std::string artifactContentHash(const std::string & bytes)
    {
        return chain::bytes32ToHex(chain::keccakString(bytes));
    }

    ArtifactStore::ArtifactStore(
        std::filesystem::path cache_root,
        std::vector<std::filesystem::path> gateways,
        std::vector<std::filesystem::path> replicas)
        : _cache_root(std::move(cache_root))
        , _gateways(std::move(gateways))
        , _replicas(std::move(replicas))
    {
        for(const auto & replica : _replicas)
        {
            if(std::ranges::find(_gateways, replica) == _gateways.end()) _gateways.push_back(replica);
        }
    }

    std::expected<std::string, std::string> ArtifactStore::cache(const std::string & bytes) const
    {
        const auto bundle = decodeArtifactBundle(bytes);
        if(!bundle) return std::unexpected(bundle.error());
        const std::string content_hash = artifactContentHash(bytes);
        if(const auto write = writeVerified(_cache_root, content_hash, bytes); !write)
        {
            return std::unexpected(write.error());
        }
        return content_hash;
    }

    std::expected<std::string, std::string> ArtifactStore::publish(const std::string & bytes) const
    {
        if(_replicas.empty()) return std::unexpected("no durable artifact replicas configured");
        const auto bundle = decodeArtifactBundle(bytes);
        if(!bundle) return std::unexpected(bundle.error());

        const std::string content_hash = artifactContentHash(bytes);
        for(const auto & replica : _replicas)
        {
            if(const auto write = writeVerified(replica, content_hash, bytes); !write)
            {
                return std::unexpected(write.error());
            }
        }
        if(const auto write = writeVerified(_cache_root, content_hash, bytes); !write)
        {
            return std::unexpected(write.error());
        }
        return content_hash;
    }

    ArtifactRead ArtifactStore::read(const std::string & content_hash) const
    {
        if(!validHash(content_hash))
        {
            return {.state = ArtifactState::INVALID, .error = "invalid artifact content hash"};
        }

        bool invalid_seen = false;
        std::string last_error;
        const auto try_root = [&](const std::filesystem::path & root) -> std::optional<std::string>
        {
            const auto path = artifactPath(root, content_hash);
            std::error_code exists_ec;
            if(!std::filesystem::exists(path, exists_ec) || exists_ec) return std::nullopt;
            const auto bytes = readFile(path);
            if(!bytes)
            {
                invalid_seen = true;
                last_error = bytes.error();
                return std::nullopt;
            }
            const auto bundle = decodeArtifactBundle(*bytes, content_hash);
            if(!bundle)
            {
                invalid_seen = true;
                last_error = bundle.error();
                return std::nullopt;
            }
            return *bytes;
        };

        if(const auto bytes = try_root(_cache_root))
        {
            return {.state = ArtifactState::AVAILABLE, .bytes = *bytes};
        }
        for(const auto & gateway : _gateways)
        {
            if(const auto bytes = try_root(gateway))
            {
                if(const auto cached = writeVerified(_cache_root, content_hash, *bytes); !cached)
                {
                    return {.state = ArtifactState::INVALID, .error = cached.error()};
                }
                return {.state = ArtifactState::AVAILABLE, .bytes = *bytes};
            }
        }

        return {
            .state = invalid_seen ? ArtifactState::INVALID : ArtifactState::MISSING,
            .error = invalid_seen ? last_error : "artifact is unavailable"};
    }
}
