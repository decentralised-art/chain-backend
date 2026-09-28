#include "unit-tests.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>

#include "artifact.hpp"
#include "chain.hpp"
#include "events_test_harness.hpp"
#include "artifact_runtime.hpp"
#include "sqlite_registry_store.hpp"

using namespace dcn;
using namespace dcn::tests::events_harness;

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

TEST(ArtifactBundle, CanonicalBytesAndExactHashRoundTrip)
{
    const auto first = pt::encodeArtifactBundle(makeBundle());
    const auto second = pt::encodeArtifactBundle(makeBundle());
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    EXPECT_EQ(*first, *second);

    const std::string hash = pt::artifactContentHash(*first);
    const auto decoded = pt::decodeArtifactBundle(*first, hash);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded->kind, pt::EntityKind::TRANSFORMATION);
    EXPECT_EQ(decoded->name, "Scale");
    EXPECT_EQ(decoded->sol_src, "return x * args[0];");

    EXPECT_FALSE(pt::decodeArtifactBundle(*first + "\n", hash));
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

TEST(ArtifactStore, DurableWriteReadBackAndInvalidBytesAreNeverReturned)
{
    const auto paths = makeTempEventsPaths("artifact_store");
    const auto bytes = pt::encodeArtifactBundle(makeBundle());
    ASSERT_TRUE(bytes);

    const auto replica_a = paths.root / "replica-a";
    const auto replica_b = paths.root / "replica-b";
    pt::ArtifactStore publisher(
        paths.root / "publisher-cache", {}, {replica_a, replica_b});
    const auto published = publisher.publish(*bytes);
    ASSERT_TRUE(published);

    pt::ArtifactStore reader(paths.root / "reader-cache", {replica_a, replica_b});
    const auto fetched = reader.read(*published);
    ASSERT_EQ(fetched.state, pt::ArtifactState::AVAILABLE);
    ASSERT_TRUE(fetched.bytes);
    EXPECT_EQ(*fetched.bytes, *bytes);

    const auto invalid_root = paths.root / "invalid";
    std::filesystem::create_directories(invalid_root);
    std::ofstream(invalid_root / (*published + ".json"), std::ios::binary) << "{}";
    const auto invalid = pt::ArtifactStore(paths.root / "empty-cache", {invalid_root}).read(*published);
    EXPECT_EQ(invalid.state, pt::ArtifactState::INVALID);
    EXPECT_FALSE(invalid.bytes);
}

TEST(ArtifactState, RegistryTracksRetryableArtifactWorkWithoutHidingEntity)
{
    const auto paths = makeTempEventsPaths("artifact_registry_state");
    registry::SQLiteRegistryStore store((paths.root / "registry.sqlite").string());

    TransformationRecord record;
    record.mutable_transformation()->set_name("Scale");
    record.mutable_transformation()->set_args_count(1);
    record.set_owner("0x1111111111111111111111111111111111111111");
    const registry::EntitySource source{
        .source_id = "eip155:1:0x2222222222222222222222222222222222222222",
        .chain_namespace = "eip155:1",
        .chain_id = 1,
        .registry_address = "0x2222222222222222222222222222222222222222",
        .content_hash = chain::bytes32ToHex(chain::keccakString("bundle")),
        .metadata_hash = chain::bytes32ToHex(pt::computeMetadataHash(pt::EntityKind::TRANSFORMATION, "Scale")),
        .runtime_code_hash = makeBundle().runtime_code_hash};
    ASSERT_TRUE(store.addTransformationVersion(source, {}, record));

    const auto due = store.getArtifactsDue(100, 10);
    ASSERT_EQ(due.size(), 1);
    EXPECT_EQ(due.front().state, pt::ArtifactState::MISSING);
    ASSERT_TRUE(store.setArtifactState(
        registry::EntityKind::TRANSFORMATION,
        source.source_id,
        "Scale",
        pt::ArtifactState::MISSING,
        200));
    EXPECT_TRUE(store.getArtifactsDue(199, 10).empty());
    const auto retry = store.getArtifactsDue(200, 10);
    ASSERT_EQ(retry.size(), 1);
    EXPECT_EQ(retry.front().attempts, 1u);
    ASSERT_TRUE(store.setArtifactState(
        registry::EntityKind::TRANSFORMATION,
        source.source_id,
        "Scale",
        pt::ArtifactState::AVAILABLE,
        0));
    EXPECT_TRUE(store.getArtifactsDue(100, 10).empty());

    const auto location = store.getEntityLocation(registry::EntityKind::TRANSFORMATION, "Scale");
    ASSERT_TRUE(location);
    EXPECT_EQ(location->source.artifact_state, pt::ArtifactState::AVAILABLE);
}

TEST(ArtifactState, RuntimeHydratesAvailableBundleIndependentlyOfEventCursor)
{
    const auto paths = makeTempEventsPaths("artifact_projector_hydration");
    const auto bytes = pt::encodeArtifactBundle(makeBundle());
    ASSERT_TRUE(bytes);

    const auto gateway = paths.root / "gateway";
    pt::ArtifactStore publisher(paths.root / "publisher-cache", {}, {gateway});
    const auto content_hash = publisher.publish(*bytes);
    ASSERT_TRUE(content_hash);

    asio::io_context io;
    registry::Registry registry(io, (paths.root / "registry.sqlite").string());
    TransformationRecord record;
    record.mutable_transformation()->set_name("Scale");
    record.mutable_transformation()->set_args_count(1);
    record.set_owner("0x1111111111111111111111111111111111111111");
    const registry::EntitySource source{
        .source_id = "eip155:1:0x2222222222222222222222222222222222222222",
        .chain_namespace = "eip155:1",
        .chain_id = 1,
        .registry_address = "0x2222222222222222222222222222222222222222",
        .content_hash = *content_hash,
        .metadata_hash = makeBundle().metadata_hash,
        .runtime_code_hash = makeBundle().runtime_code_hash};
    ASSERT_TRUE(runAwaitable(io, registry.addTransformationVersion(source, {}, record)));

    const pt::ArtifactStore reader(paths.root / "reader-cache", {gateway});
    registry::ArtifactRuntime runtime(io, registry, reader);
    EXPECT_EQ(runAwaitable(io, runtime.hydrateOnce(1'000)), 1u);

    const auto location = runAwaitable(
        io, registry.getEntityLocation(registry::EntityKind::TRANSFORMATION, "Scale"));
    ASSERT_TRUE(location);
    EXPECT_EQ(location->source.artifact_state, pt::ArtifactState::AVAILABLE);
    EXPECT_TRUE(std::filesystem::exists(paths.root / "reader-cache" / (*content_hash + ".json")));

    registry::EntitySource mismatched_source = source;
    mismatched_source.runtime_code_hash = chain::bytes32ToHex(chain::keccakString("different runtime"));
    ASSERT_TRUE(runAwaitable(
        io, registry.addTransformationVersion(mismatched_source, {}, record)));
    EXPECT_EQ(runAwaitable(io, runtime.hydrateOnce(2'000)), 0u);
    const auto invalid_location = runAwaitable(
        io, registry.getEntityLocation(registry::EntityKind::TRANSFORMATION, "Scale"));
    ASSERT_TRUE(invalid_location);
    EXPECT_EQ(invalid_location->source.artifact_state, pt::ArtifactState::INVALID);
}
