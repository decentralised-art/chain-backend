#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "publication.hpp"

namespace dcn::pt
{
    constexpr std::size_t MAX_ARTIFACT_BYTES = 4 * 1024 * 1024;

    struct ArtifactDependency
    {
        EntityKind kind = EntityKind::CONNECTOR;
        std::string name;
        std::string content_hash;
    };

    /// The canonical description of an entity this server publishes. Its keccak256 is the
    /// publication's content hash, the commitment the PT protocol requires; the bundle
    /// itself is never stored or served.
    struct ArtifactBundle
    {
        EntityKind kind = EntityKind::CONNECTOR;
        std::string name{};
        nlohmann::json definition{};
        std::string sol_src{};
        std::string generated_solidity{};
        std::string pt_revision{};
        std::string solc_version{};
        std::string evm_version = "shanghai";
        bool optimizer = true;
        std::uint32_t optimizer_runs = 200;
        bool via_ir = true;
        std::vector<std::string> imports{};
        // Hash of compiler-emitted init bytecode before constructor arguments are appended.
        std::string creation_code_hash{};
        std::string runtime_code_hash{};
        std::string metadata_hash{};
        std::vector<ArtifactDependency> dependencies{};
    };

    /// Canonical bytes of a valid bundle; an error for an invalid or oversized one.
    std::expected<std::string, std::string> encodeArtifactBundle(const ArtifactBundle & bundle);
    std::string artifactContentHash(const std::string & bytes);
}
