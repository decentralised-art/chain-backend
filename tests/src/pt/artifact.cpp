#include "unit-tests.hpp"

#include <algorithm>

#include "artifact.hpp"
#include "chain.hpp"

using namespace dcn;

namespace
{
    pt::ArtifactBundle makeBundle()
    {
        const auto metadata = pt::computeMetadataHash(pt::EntityKind::TRANSFORMATION, "Scale");
        return pt::ArtifactBundle{
            .kind = pt::EntityKind::TRANSFORMATION,
            .name = "Scale",
            .definition = {
                {"owner", "0x1111111111111111111111111111111111111111"},
                {"transformation", {{"argsCount", 1}, {"name", "Scale"}, {"solSrc", "return x * args[0];"}}}},
            .sol_src = "return x * args[0];",
            .generated_solidity = "contract Scale {}",
            .pt_revision = chain::bytes32ToHex(chain::keccakString("pt sources")),
            .solc_version = "0.8.30",
            .imports = {"TransformationBase.sol"},
            .creation_code_hash = chain::bytes32ToHex(chain::keccakString("creation")),
            .runtime_code_hash = chain::bytes32ToHex(chain::keccakString("runtime")),
            .metadata_hash = chain::bytes32ToHex(metadata)};
    }
}

// The content hash a publication carries is the keccak256 of the bundle's canonical bytes,
// so the same bundle always yields the same hash and any change yields another.
TEST(ArtifactBundle, CanonicalBytesGiveAStableContentHash)
{
    const auto first = pt::encodeArtifactBundle(makeBundle());
    const auto second = pt::encodeArtifactBundle(makeBundle());
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    EXPECT_EQ(*first, *second);
    EXPECT_EQ(pt::artifactContentHash(*first), chain::bytes32ToHex(pt::computeContentHash(*first)));

    auto changed = makeBundle();
    changed.sol_src = "return x * args[0] + 1;";
    const auto other = pt::encodeArtifactBundle(changed);
    ASSERT_TRUE(other);
    EXPECT_NE(pt::artifactContentHash(*other), pt::artifactContentHash(*first));
}

TEST(ArtifactBundle, CanonicalizesImportAndDependencyOrder)
{
    auto first_bundle = makeBundle();
    first_bundle.imports = {"z.sol", "a.sol", "a.sol"};
    first_bundle.dependencies = {
        {.kind = pt::EntityKind::TRANSFORMATION, .name = "Z", .content_hash = first_bundle.runtime_code_hash},
        {.kind = pt::EntityKind::CONDITION, .name = "A", .content_hash = first_bundle.creation_code_hash}};
    auto second_bundle = first_bundle;
    std::ranges::reverse(second_bundle.dependencies);

    const auto first = pt::encodeArtifactBundle(first_bundle);
    const auto second = pt::encodeArtifactBundle(second_bundle);
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    EXPECT_EQ(*first, *second);
}

// Only a complete bundle is hashed into a publication.
TEST(ArtifactBundle, RefusesAnIncompleteBundle)
{
    auto no_source = makeBundle();
    no_source.sol_src.clear();
    EXPECT_FALSE(pt::encodeArtifactBundle(no_source));

    auto bad_hash = makeBundle();
    bad_hash.runtime_code_hash = "0x1234";
    EXPECT_FALSE(pt::encodeArtifactBundle(bad_hash));

    auto wrong_metadata = makeBundle();
    wrong_metadata.metadata_hash = chain::bytes32ToHex(chain::keccakString("other"));
    EXPECT_FALSE(pt::encodeArtifactBundle(wrong_metadata));
}
