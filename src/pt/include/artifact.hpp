#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "publication.hpp"

namespace dcn::pt
{
    constexpr std::size_t MAX_ARTIFACT_BYTES = 4 * 1024 * 1024;

    enum class ArtifactState : std::uint8_t
    {
        AVAILABLE,
        MISSING,
        INVALID
    };

    std::string_view artifactStateName(ArtifactState state);

    struct ArtifactDependency
    {
        EntityKind kind = EntityKind::CONNECTOR;
        std::string name;
        std::string content_hash;
    };

    struct ArtifactBundle
    {
        EntityKind kind = EntityKind::CONNECTOR;
        std::string name;
        nlohmann::json definition;
        std::string sol_src;
        std::string generated_solidity;
        std::string pt_revision;
        std::string solc_version;
        std::string evm_version = "shanghai";
        bool optimizer = true;
        std::uint32_t optimizer_runs = 200;
        bool via_ir = true;
        std::vector<std::string> imports;
        // Hash of compiler-emitted init bytecode before constructor arguments are appended.
        std::string creation_code_hash;
        std::string runtime_code_hash;
        std::string metadata_hash;
        std::vector<ArtifactDependency> dependencies;
    };

    std::expected<std::string, std::string> encodeArtifactBundle(const ArtifactBundle & bundle);
    std::expected<ArtifactBundle, std::string> decodeArtifactBundle(
        const std::string & bytes,
        const std::string & expected_content_hash = {});
    std::string artifactContentHash(const std::string & bytes);

    struct ArtifactRead
    {
        ArtifactState state = ArtifactState::MISSING;
        std::optional<std::string> bytes;
        std::string error;
    };

    class ArtifactStore
    {
        public:
            ArtifactStore(
                std::filesystem::path cache_root,
                std::vector<std::filesystem::path> gateways = {},
                std::vector<std::filesystem::path> replicas = {});

            // Stores a verified bundle in the local read-through cache.
            std::expected<std::string, std::string> cache(const std::string & bytes) const;

            // Stores a verified bundle in every durable replica and the cache; each copy is
            // written atomically, flushed to disk and read back before this succeeds.
            std::expected<std::string, std::string> publish(const std::string & bytes) const;

            // The bundle with `content_hash` from the cache, else from a gateway directory
            // (then cached). Bytes are only returned after their hash and structure verify.
            ArtifactRead read(const std::string & content_hash) const;

        private:
            std::filesystem::path _cache_root;
            std::vector<std::filesystem::path> _gateways;
            std::vector<std::filesystem::path> _replicas;
    };
}
