#include "unit-tests.hpp"

#include <cstring>
#include <expected>
#include <fstream>
#include <filesystem>
#include <format>
#include <future>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <vector>

#ifndef DECENTRALISED_ART_TEST_SOLC_PATH
    #error "DECENTRALISED_ART_TEST_SOLC_PATH is not defined"
#endif

#ifndef DECENTRALISED_ART_TEST_PT_PATH
    #error "DECENTRALISED_ART_TEST_PT_PATH is not defined"
#endif

using namespace dcn;
using namespace dcn::tests;

// Authorization and event invariants of the registry-owned publication path.
// Every case here drives the real contracts through the in-process EVM.
namespace
{
    template<class AwaitableT>
    auto runAwaitable(asio::io_context & io_context, AwaitableT awaitable)
    {
        auto future = asio::co_spawn(io_context, std::move(awaitable), asio::use_future);
        io_context.restart();
        io_context.run();
        return future.get();
    }

    std::filesystem::path solcPath()
    {
        return std::filesystem::path(DECENTRALISED_ART_TEST_SOLC_PATH);
    }

    std::filesystem::path ptPath()
    {
        return std::filesystem::path(DECENTRALISED_ART_TEST_PT_PATH);
    }

    std::filesystem::path makeStoragePath(const std::string & suffix)
    {
        const std::filesystem::path root =
            std::filesystem::temp_directory_path() / std::format("dcn_pt_publication_{}", suffix);
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        for(const char * dir : {"transformations", "conditions", "connectors"})
        {
            std::filesystem::create_directories(root / dir / "build", ec);
        }
        return root;
    }

    chain::Address makeAddressFromByte(const std::uint8_t value)
    {
        chain::Address address{};
        address.bytes[19] = value;
        return address;
    }

    struct PublicationFixture
    {
        asio::io_context io_context;
        std::unique_ptr<evm::EVM> evm_instance;
        registry::Registry registry;
        std::filesystem::path storage_path;
        chain::Address owner{};

        explicit PublicationFixture(const std::string & suffix)
            : registry(io_context)
            , storage_path(makeStoragePath(suffix))
            , owner(makeAddressFromByte(0x61))
        {
            evm_instance = std::make_unique<evm::EVM>(io_context, EVMC_SHANGHAI, solcPath(), ptPath());
            io_context.run();

            (void)runAwaitable(io_context, evm_instance->addAccount(owner, evm::DEFAULT_GAS_LIMIT));
            (void)runAwaitable(io_context, evm_instance->setGas(owner, evm::DEFAULT_GAS_LIMIT));
        }

        std::expected<chain::Address, pt::PTDeployError> publishTransformation(
            const std::string & name,
            const chain::Address & record_owner)
        {
            TransformationRecord record;
            record.set_owner(chain::addressToHex(record_owner));
            record.mutable_transformation()->set_name(name);
            record.mutable_transformation()->set_sol_src("return x;");

            return runAwaitable(
                io_context,
                loader::deployTransformation(*evm_instance, registry, record, storage_path));
        }

        std::vector<std::uint8_t> encodeRegistryCall(
            const std::string & signature,
            const std::vector<pt::abi::Component> & arguments)
        {
            std::vector<std::uint8_t> call = chain::constructSelector(signature);
            const std::vector<std::uint8_t> body = pt::abi::encodeTuple(arguments);
            call.insert(call.end(), body.begin(), body.end());
            return call;
        }

        std::expected<std::vector<std::uint8_t>, chain::ExecuteError> callRegistry(
            const chain::Address & caller,
            std::vector<std::uint8_t> call)
        {
            (void)runAwaitable(io_context, evm_instance->addAccount(caller, evm::DEFAULT_GAS_LIMIT));
            (void)runAwaitable(io_context, evm_instance->setGas(caller, evm::DEFAULT_GAS_LIMIT));
            return runAwaitable(
                io_context,
                evm_instance->execute(
                    caller,
                    evm_instance->getRegistryAddress(),
                    std::move(call),
                    evm::DEFAULT_GAS_LIMIT,
                    0));
        }
    };

    bool reverted(const auto & result)
    {
        return !result.has_value() && result.error().kind == chain::ExecuteError::Kind::TRANSACTION_REVERTED;
    }
}

// The publication path is the only way to register: the removed self-registration ABI
// no longer resolves, so a direct call falls through to the reverting fallback.
TEST_F(UnitTest, PT_Publication_RemovedSelfRegistrationAbi_Reverts)
{
    ASSERT_TRUE(std::filesystem::exists(solcPath()));
    ASSERT_TRUE(std::filesystem::exists(ptPath() / "contracts"));

    PublicationFixture fixture("removed_abi");

    const auto result = fixture.callRegistry(
        fixture.owner,
        fixture.encodeRegistryCall(
            "registerTransformation(string,address,(address,uint32))",
            {pt::abi::stringComponent("Ghost"),
             pt::abi::addressComponent(makeAddressFromByte(0x71)),
             pt::abi::addressComponent(fixture.owner),
             pt::abi::uintComponent(0)}));

    EXPECT_TRUE(reverted(result));
}

// A successful publication registers the entity, records the owner in the registry
// (not in the entity), and consumes the owner's publication nonce.
TEST_F(UnitTest, PT_Publication_RegistersEntityAndConsumesNonce)
{
    ASSERT_TRUE(std::filesystem::exists(solcPath()));
    ASSERT_TRUE(std::filesystem::exists(ptPath() / "contracts"));

    PublicationFixture fixture("register_nonce");

    const auto nonce_before = fixture.callRegistry(
        fixture.owner, pt::encodePublicationNonceCall(fixture.owner));
    ASSERT_TRUE(nonce_before.has_value());
    ASSERT_GE(nonce_before->size(), 32u);
    EXPECT_EQ(chain::readUint256(*nonce_before, 0), 0u);

    const auto published = fixture.publishTransformation("PublishedTransform", fixture.owner);
    ASSERT_TRUE(published.has_value())
        << std::format("publish failed: kind={}", static_cast<int>(published.error().kind));
    EXPECT_NE(*published, chain::Address{});

    const auto nonce_after = fixture.callRegistry(
        fixture.owner, pt::encodePublicationNonceCall(fixture.owner));
    ASSERT_TRUE(nonce_after.has_value());
    EXPECT_EQ(chain::readUint256(*nonce_after, 0), 1u);

    const auto entity_owner = fixture.callRegistry(
        fixture.owner,
        fixture.encodeRegistryCall(
            "getEntityOwner(uint8,string)",
            {pt::abi::uintComponent(static_cast<std::uint64_t>(pt::EntityKind::TRANSFORMATION)),
             pt::abi::stringComponent("PublishedTransform")}));
    ASSERT_TRUE(entity_owner.has_value());
    const auto owner_address = chain::readAddressWord(*entity_owner);
    ASSERT_TRUE(owner_address.has_value());
    EXPECT_EQ(*owner_address, fixture.owner);

    // The published entity itself exposes no ownership surface.
    const auto entity_get_owner = runAwaitable(
        fixture.io_context, evm::fetchOwner(*fixture.evm_instance, *published));
    EXPECT_FALSE(entity_get_owner.has_value());
}

// Publishing the same name twice is refused, so a name is claimed exactly once.
TEST_F(UnitTest, PT_Publication_DuplicateName_IsRefused)
{
    ASSERT_TRUE(std::filesystem::exists(solcPath()));
    ASSERT_TRUE(std::filesystem::exists(ptPath() / "contracts"));

    PublicationFixture fixture("duplicate_name");

    ASSERT_TRUE(fixture.publishTransformation("DuplicateTransform", fixture.owner).has_value());

    const auto second = fixture.publishTransformation("DuplicateTransform", fixture.owner);
    ASSERT_FALSE(second.has_value());
    EXPECT_EQ(second.error().kind, pt::PTDeployError::Kind::TRANSFORMATION_ALREADY_REGISTERED);
}

// Removal is owner-only and carries the recorded hashes, so replay never has to look
// up mutable state to understand the event.
TEST_F(UnitTest, PT_Publication_ClearTransformation_IsOwnerOnly)
{
    ASSERT_TRUE(std::filesystem::exists(solcPath()));
    ASSERT_TRUE(std::filesystem::exists(ptPath() / "contracts"));

    PublicationFixture fixture("clear_authorization");
    ASSERT_TRUE(fixture.publishTransformation("ClearableTransform", fixture.owner).has_value());

    const chain::Address stranger = makeAddressFromByte(0x62);
    const auto stranger_clear = fixture.callRegistry(
        stranger,
        fixture.encodeRegistryCall(
            "clearTransformation(string)", {pt::abi::stringComponent("ClearableTransform")}));
    EXPECT_TRUE(reverted(stranger_clear));

    const auto still_present = fixture.callRegistry(
        fixture.owner,
        fixture.encodeRegistryCall(
            "containsTransformation(string)", {pt::abi::stringComponent("ClearableTransform")}));
    ASSERT_TRUE(still_present.has_value());
    ASSERT_GE(still_present->size(), 32u);
    EXPECT_EQ(chain::readUint256(*still_present, 0), 1u);

    const auto owner_clear = fixture.callRegistry(
        fixture.owner,
        fixture.encodeRegistryCall(
            "clearTransformation(string)", {pt::abi::stringComponent("ClearableTransform")}));
    ASSERT_TRUE(owner_clear.has_value());

    const auto after_clear = fixture.callRegistry(
        fixture.owner,
        fixture.encodeRegistryCall(
            "containsTransformation(string)", {pt::abi::stringComponent("ClearableTransform")}));
    ASSERT_TRUE(after_clear.has_value());
    EXPECT_EQ(chain::readUint256(*after_clear, 0), 0u);
}

// Entity ownership moves through the registry, is owner-only, and rejects the zero address.
TEST_F(UnitTest, PT_Publication_ChangeEntityOwner_IsOwnerOnlyAndRejectsZero)
{
    ASSERT_TRUE(std::filesystem::exists(solcPath()));
    ASSERT_TRUE(std::filesystem::exists(ptPath() / "contracts"));

    PublicationFixture fixture("change_entity_owner");
    ASSERT_TRUE(fixture.publishTransformation("OwnedTransform", fixture.owner).has_value());

    const chain::Address stranger = makeAddressFromByte(0x63);
    const chain::Address new_owner = makeAddressFromByte(0x64);

    const auto kind_arg =
        pt::abi::uintComponent(static_cast<std::uint64_t>(pt::EntityKind::TRANSFORMATION));

    const auto stranger_change = fixture.callRegistry(
        stranger,
        fixture.encodeRegistryCall(
            "changeEntityOwner(uint8,string,address)",
            {kind_arg, pt::abi::stringComponent("OwnedTransform"), pt::abi::addressComponent(new_owner)}));
    EXPECT_TRUE(reverted(stranger_change));

    const auto zero_change = fixture.callRegistry(
        fixture.owner,
        fixture.encodeRegistryCall(
            "changeEntityOwner(uint8,string,address)",
            {kind_arg, pt::abi::stringComponent("OwnedTransform"), pt::abi::addressComponent(chain::Address{})}));
    EXPECT_TRUE(reverted(zero_change));

    const auto owner_change = fixture.callRegistry(
        fixture.owner,
        fixture.encodeRegistryCall(
            "changeEntityOwner(uint8,string,address)",
            {kind_arg, pt::abi::stringComponent("OwnedTransform"), pt::abi::addressComponent(new_owner)}));
    ASSERT_TRUE(owner_change.has_value());

    const auto entity_owner = fixture.callRegistry(
        fixture.owner,
        fixture.encodeRegistryCall(
            "getEntityOwner(uint8,string)", {kind_arg, pt::abi::stringComponent("OwnedTransform")}));
    ASSERT_TRUE(entity_owner.has_value());
    const auto owner_address = chain::readAddressWord(*entity_owner);
    ASSERT_TRUE(owner_address.has_value());
    EXPECT_EQ(*owner_address, new_owner);
}

// A publication whose creation-code hash does not match the submitted bytes is refused
// before anything is deployed.
TEST_F(UnitTest, PT_Publication_CreationCodeHashMismatch_Reverts)
{
    ASSERT_TRUE(std::filesystem::exists(solcPath()));
    ASSERT_TRUE(std::filesystem::exists(ptPath() / "contracts"));

    PublicationFixture fixture("creation_code_mismatch");

    pt::Publication publication;
    publication.kind = pt::EntityKind::TRANSFORMATION;
    publication.name = "MismatchedTransform";
    publication.owner = fixture.owner;
    publication.content_hash = pt::computeContentHash("contract X {}");
    publication.metadata_hash = pt::computeMetadataHash(pt::EntityKind::TRANSFORMATION, publication.name);
    publication.creation_code_hash = chain::keccakString("not the creation code");
    publication.runtime_code_hash = chain::keccakString("not the runtime code");
    publication.deadline = std::numeric_limits<std::uint64_t>::max();

    const std::vector<std::uint8_t> creation_code{0x60, 0x00, 0x60, 0x00, 0xF3};
    const auto result = fixture.callRegistry(
        fixture.owner,
        pt::encodePublishTransformationCall(
            std::span<const std::uint8_t>(creation_code.data(), creation_code.size()),
            publication,
            {}));

    EXPECT_TRUE(reverted(result));
}

// An unauthenticated caller cannot publish on someone else's behalf: an empty signature
// is only accepted from the owner.
TEST_F(UnitTest, PT_Publication_EmptySignatureFromNonOwner_Reverts)
{
    ASSERT_TRUE(std::filesystem::exists(solcPath()));
    ASSERT_TRUE(std::filesystem::exists(ptPath() / "contracts"));

    PublicationFixture fixture("unauthenticated_publisher");

    pt::Publication publication;
    publication.kind = pt::EntityKind::TRANSFORMATION;
    publication.name = "StolenTransform";
    publication.owner = fixture.owner;
    publication.content_hash = chain::keccakString("content");
    publication.metadata_hash = chain::keccakString("metadata");
    publication.creation_code_hash = chain::keccakString("creation");
    publication.runtime_code_hash = chain::keccakString("runtime");
    publication.deadline = std::numeric_limits<std::uint64_t>::max();

    const std::vector<std::uint8_t> creation_code{0x60, 0x00, 0x60, 0x00, 0xF3};
    const auto result = fixture.callRegistry(
        makeAddressFromByte(0x65),
        pt::encodePublishTransformationCall(
            std::span<const std::uint8_t>(creation_code.data(), creation_code.size()),
            publication,
            {}));

    EXPECT_TRUE(reverted(result));
}

// The signed deadline is enforced on chain, so an authorization cannot be held back and
// replayed later.
TEST_F(UnitTest, PT_Publication_ExpiredDeadline_Reverts)
{
    ASSERT_TRUE(std::filesystem::exists(solcPath()));
    ASSERT_TRUE(std::filesystem::exists(ptPath() / "contracts"));

    PublicationFixture fixture("expired_deadline");

    pt::Publication publication;
    publication.kind = pt::EntityKind::TRANSFORMATION;
    publication.name = "ExpiredTransform";
    publication.owner = fixture.owner;
    publication.content_hash = chain::keccakString("content");
    publication.metadata_hash = chain::keccakString("metadata");
    publication.creation_code_hash = chain::keccakString("creation");
    publication.runtime_code_hash = chain::keccakString("runtime");
    publication.deadline = 0;

    const std::vector<std::uint8_t> creation_code{0x60, 0x00, 0x60, 0x00, 0xF3};
    const auto result = fixture.callRegistry(
        fixture.owner,
        pt::encodePublishTransformationCall(
            std::span<const std::uint8_t>(creation_code.data(), creation_code.size()),
            publication,
            {}));

    EXPECT_TRUE(reverted(result));
}

// Each authorization is spent once: a publication carrying an already-consumed nonce is
// refused even under a fresh name.
TEST_F(UnitTest, PT_Publication_ReplayedNonce_Reverts)
{
    ASSERT_TRUE(std::filesystem::exists(solcPath()));
    ASSERT_TRUE(std::filesystem::exists(ptPath() / "contracts"));

    PublicationFixture fixture("replayed_nonce");

    const auto published = fixture.publishTransformation("FirstTransform", fixture.owner);
    ASSERT_TRUE(published.has_value())
        << std::format("publish failed: kind={}", static_cast<int>(published.error().kind));

    pt::Publication publication;
    publication.kind = pt::EntityKind::TRANSFORMATION;
    publication.name = "SecondTransform";
    publication.owner = fixture.owner;
    publication.content_hash = chain::keccakString("content");
    publication.metadata_hash = chain::keccakString("metadata");
    publication.creation_code_hash = chain::keccakString("creation");
    publication.runtime_code_hash = chain::keccakString("runtime");
    publication.deadline = std::numeric_limits<std::uint64_t>::max();
    publication.nonce = 0; // already consumed by the first publication

    const std::vector<std::uint8_t> creation_code{0x60, 0x00, 0x60, 0x00, 0xF3};
    const auto result = fixture.callRegistry(
        fixture.owner,
        pt::encodePublishTransformationCall(
            std::span<const std::uint8_t>(creation_code.data(), creation_code.size()),
            publication,
            {}));

    EXPECT_TRUE(reverted(result));
}

// Upgrade ownership cannot be handed to the zero address, which would strand the proxy.
TEST_F(UnitTest, PT_Ownable_ChangeOwnerToZero_RevertsAndKeepsOwner)
{
    ASSERT_TRUE(std::filesystem::exists(solcPath()));
    ASSERT_TRUE(std::filesystem::exists(ptPath() / "contracts"));

    PublicationFixture fixture("ownable_zero");

    chain::Address deployer{};
    std::memcpy(deployer.bytes + (20 - 7), "genesis", 7);

    const auto result = fixture.callRegistry(
        deployer,
        fixture.encodeRegistryCall("changeOwner(address)", {pt::abi::addressComponent(chain::Address{})}));
    EXPECT_TRUE(reverted(result));

    const auto owner_after = fixture.callRegistry(
        deployer, fixture.encodeRegistryCall("getOwner()", {}));
    ASSERT_TRUE(owner_after.has_value());
    const auto owner_address = chain::readAddressWord(*owner_after);
    ASSERT_TRUE(owner_address.has_value());
    EXPECT_EQ(*owner_address, deployer);
}

namespace
{
    pt::abi::Component publicationArgument(const pt::Publication & publication)
    {
        // The struct holds a string, so it is a dynamic tuple component.
        return pt::abi::Component{.dynamic = true, .data = pt::abi::encodeTuple({
            pt::abi::uintComponent(static_cast<std::uint64_t>(publication.kind)),
            pt::abi::stringComponent(publication.name),
            pt::abi::addressComponent(publication.owner),
            pt::abi::bytes32Component(publication.content_hash),
            pt::abi::bytes32Component(publication.metadata_hash),
            pt::abi::bytes32Component(publication.creation_code_hash),
            pt::abi::bytes32Component(publication.runtime_code_hash),
            pt::abi::uintComponent(publication.nonce),
            pt::abi::uintComponent(publication.deadline)})};
    }

    const std::string OWNER_KEY =
        "0x4c0883a69102937d6231471b5dbb6204fe5129617082792ae468d01a3f362318";
    const std::string OTHER_KEY =
        "0x2a871d0798f97d79848a013d4936a73bf4cc922c825d33c1cf7073dff6d409c6";

    std::shared_ptr<publish::Signer> signerFromHexKey(const std::string & hex_key)
    {
        const std::filesystem::path key_path =
            std::filesystem::temp_directory_path() / std::format("dcn_pt_key_{}.hex", hex_key.substr(2, 8));
        std::ofstream out(key_path, std::ios::trunc);
        out << hex_key;
        out.close();

        auto signer = publish::Signer::fromKeyFile(key_path);
        if(!signer) throw std::runtime_error(signer.error());
        return *signer;
    }

    // Registry-side digest for the same publication: the server must agree with it
    // exactly or every signed publication it builds would be rejected.
    evmc::bytes32 onChainDigest(PublicationFixture & fixture, const pt::Publication & publication)
    {
        const auto result = fixture.callRegistry(
            fixture.owner,
            fixture.encodeRegistryCall(
                "publicationDigest((uint8,string,address,bytes32,bytes32,bytes32,bytes32,uint256,uint256))",
                {publicationArgument(publication)}));
        if(!result || result->size() < 32) return {};

        evmc::bytes32 digest{};
        std::copy(result->begin(), result->begin() + 32, digest.bytes);
        return digest;
    }
}

// The server-side EIP-712 digest must be byte-identical to the registry's, so a
// publication authorization produced off chain verifies on chain.
TEST_F(UnitTest, PT_Publication_ServerDigestMatchesRegistryDigest)
{
    ASSERT_TRUE(std::filesystem::exists(solcPath()));
    ASSERT_TRUE(std::filesystem::exists(ptPath() / "contracts"));

    PublicationFixture fixture("digest_agreement");

    pt::Publication publication;
    publication.kind = pt::EntityKind::TRANSFORMATION;
    publication.name = "DigestTransform";
    publication.owner = fixture.owner;
    publication.content_hash = chain::keccakString("content");
    publication.metadata_hash = chain::keccakString("metadata");
    publication.creation_code_hash = chain::keccakString("creation");
    publication.runtime_code_hash = chain::keccakString("runtime");
    publication.nonce = 0;
    publication.deadline = 1900000000;

    const evmc::bytes32 expected = onChainDigest(fixture, publication);
    ASSERT_NE(expected, evmc::bytes32{});

    // The local EVM reports chain id 0, which is what the domain separator must bind.
    const evmc::bytes32 computed = pt::publicationDigest(
        0, fixture.evm_instance->getRegistryAddress(), publication);
    EXPECT_EQ(computed, expected);

    // A signature over that digest recovers to the signing account.
    const auto signer = signerFromHexKey(OWNER_KEY);
    const auto signature = signer->signDigest(expected);
    ASSERT_TRUE(signature.has_value());
    const auto recovered = pt::recoverSigner(expected, *signature);
    ASSERT_TRUE(recovered.has_value());
    EXPECT_EQ(*recovered, signer->address());
}

// A third party may submit a publication the owner authorized off chain; the registry
// records the signed owner, not the caller.
TEST_F(UnitTest, PT_Publication_ThirdPartyCallerWithOwnerSignature_Publishes)
{
    ASSERT_TRUE(std::filesystem::exists(solcPath()));
    ASSERT_TRUE(std::filesystem::exists(ptPath() / "contracts"));

    PublicationFixture fixture("signed_publication");

    const auto owner_signer = signerFromHexKey(OWNER_KEY);
    const chain::Address entity_owner = owner_signer->address();

    TransformationRecord record;
    record.set_owner(chain::addressToHex(entity_owner));
    record.mutable_transformation()->set_name("SignedTransform");
    record.mutable_transformation()->set_sol_src("return x;");

    auto prepared = runAwaitable(
        fixture.io_context,
        loader::prepareTransformationPublication(*fixture.evm_instance, record, fixture.storage_path));
    ASSERT_TRUE(prepared.has_value())
        << std::format("prepare failed: kind={}", static_cast<int>(prepared.error().kind));

    pt::Publication & publication = prepared->publication;
    publication.nonce = 0;

    const evmc::bytes32 digest = pt::publicationDigest(
        0, fixture.evm_instance->getRegistryAddress(), publication);
    ASSERT_EQ(digest, onChainDigest(fixture, publication));

    const auto signature = owner_signer->signDigest(digest);
    ASSERT_TRUE(signature.has_value());

    const chain::Address relayer = makeAddressFromByte(0x66);
    const auto published = fixture.callRegistry(
        relayer,
        pt::encodePublishTransformationCall(
            std::span<const std::uint8_t>(prepared->creation_code.data(), prepared->creation_code.size()),
            publication,
            std::span<const std::uint8_t>(signature->data(), signature->size())));
    ASSERT_TRUE(published.has_value());

    const auto recorded_owner = fixture.callRegistry(
        relayer,
        fixture.encodeRegistryCall(
            "getEntityOwner(uint8,string)",
            {pt::abi::uintComponent(static_cast<std::uint64_t>(pt::EntityKind::TRANSFORMATION)),
             pt::abi::stringComponent("SignedTransform")}));
    ASSERT_TRUE(recorded_owner.has_value());
    const auto owner_address = chain::readAddressWord(*recorded_owner);
    ASSERT_TRUE(owner_address.has_value());
    EXPECT_EQ(*owner_address, entity_owner);

    // The authorization is single use: the nonce advanced past the signed value.
    const auto nonce_after = fixture.callRegistry(
        relayer, pt::encodePublicationNonceCall(entity_owner));
    ASSERT_TRUE(nonce_after.has_value());
    EXPECT_EQ(chain::readUint256(*nonce_after, 0), 1u);
}

// A signature from any other key is refused, so a publication cannot be attributed to an
// owner who never authorized it.
TEST_F(UnitTest, PT_Publication_WrongSigner_Reverts)
{
    ASSERT_TRUE(std::filesystem::exists(solcPath()));
    ASSERT_TRUE(std::filesystem::exists(ptPath() / "contracts"));

    PublicationFixture fixture("wrong_signer");

    const auto owner_signer = signerFromHexKey(OWNER_KEY);
    const auto other_signer = signerFromHexKey(OTHER_KEY);
    ASSERT_NE(owner_signer->address(), other_signer->address());

    TransformationRecord record;
    record.set_owner(chain::addressToHex(owner_signer->address()));
    record.mutable_transformation()->set_name("ForgedTransform");
    record.mutable_transformation()->set_sol_src("return x;");

    auto prepared = runAwaitable(
        fixture.io_context,
        loader::prepareTransformationPublication(*fixture.evm_instance, record, fixture.storage_path));
    ASSERT_TRUE(prepared.has_value());

    pt::Publication & publication = prepared->publication;
    publication.nonce = 0;

    const evmc::bytes32 digest = pt::publicationDigest(
        0, fixture.evm_instance->getRegistryAddress(), publication);
    const auto forged = other_signer->signDigest(digest);
    ASSERT_TRUE(forged.has_value());

    const auto result = fixture.callRegistry(
        makeAddressFromByte(0x66),
        pt::encodePublishTransformationCall(
            std::span<const std::uint8_t>(prepared->creation_code.data(), prepared->creation_code.size()),
            publication,
            std::span<const std::uint8_t>(forged->data(), forged->size())));

    EXPECT_TRUE(reverted(result));
}

// The registry validates the deployed runtime after CREATE2 returns: a publication whose
// signed runtime hash does not describe the deployed code is refused.
TEST_F(UnitTest, PT_Publication_RuntimeCodeHashMismatch_Reverts)
{
    ASSERT_TRUE(std::filesystem::exists(solcPath()));
    ASSERT_TRUE(std::filesystem::exists(ptPath() / "contracts"));

    PublicationFixture fixture("runtime_code_mismatch");

    TransformationRecord record;
    record.set_owner(chain::addressToHex(fixture.owner));
    record.mutable_transformation()->set_name("BadRuntimeTransform");
    record.mutable_transformation()->set_sol_src("return x;");

    auto prepared = runAwaitable(
        fixture.io_context,
        loader::prepareTransformationPublication(*fixture.evm_instance, record, fixture.storage_path));
    ASSERT_TRUE(prepared.has_value());

    pt::Publication & publication = prepared->publication;
    publication.nonce = 0;
    publication.runtime_code_hash = chain::keccakString("not the deployed runtime");

    const auto result = fixture.callRegistry(
        fixture.owner,
        pt::encodePublishTransformationCall(
            std::span<const std::uint8_t>(prepared->creation_code.data(), prepared->creation_code.size()),
            publication,
            {}));

    EXPECT_TRUE(reverted(result));
}

// The CREATE2 salt carries the publication nonce, so a cleared name can be published
// again with the same content: it lands at a fresh address instead of colliding with
// the code still deployed at the old one.
TEST_F(UnitTest, PT_Publication_ClearedNameRepublishesWithTheSameContent)
{
    ASSERT_TRUE(std::filesystem::exists(solcPath()));
    ASSERT_TRUE(std::filesystem::exists(ptPath() / "contracts"));

    PublicationFixture fixture("republish_cleared");

    const auto first = fixture.publishTransformation("RepublishedTransform", fixture.owner);
    ASSERT_TRUE(first.has_value());

    const auto cleared = fixture.callRegistry(
        fixture.owner,
        fixture.encodeRegistryCall("clearTransformation(string)", {pt::abi::stringComponent("RepublishedTransform")}));
    ASSERT_TRUE(cleared.has_value());

    const auto second = fixture.publishTransformation("RepublishedTransform", fixture.owner);
    ASSERT_TRUE(second.has_value())
        << std::format("republish failed: kind={}", static_cast<int>(second.error().kind));
    EXPECT_NE(*second, *first);
}

// Only a connector publication carries a registration; any other kind must bind zero so
// a registration hash cannot be smuggled into a transformation's authorization.
TEST_F(UnitTest, PT_Publication_TransformationWithRegistrationHash_Reverts)
{
    ASSERT_TRUE(std::filesystem::exists(solcPath()));
    ASSERT_TRUE(std::filesystem::exists(ptPath() / "contracts"));

    PublicationFixture fixture("transformation_registration_hash");

    TransformationRecord record;
    record.set_owner(chain::addressToHex(fixture.owner));
    record.mutable_transformation()->set_name("RegisteredTransform");
    record.mutable_transformation()->set_sol_src("return x;");

    auto prepared = runAwaitable(
        fixture.io_context,
        loader::prepareTransformationPublication(*fixture.evm_instance, record, fixture.storage_path));
    ASSERT_TRUE(prepared.has_value());

    pt::Publication & publication = prepared->publication;
    publication.nonce = 0;
    publication.registration_hash = chain::keccakString("registration");

    const auto result = fixture.callRegistry(
        fixture.owner,
        pt::encodePublishTransformationCall(
            std::span<const std::uint8_t>(prepared->creation_code.data(), prepared->creation_code.size()),
            publication,
            {}));

    EXPECT_TRUE(reverted(result));
}

// The arrays ConnectorAdded carries are bound by registrationHash: a relayer or front-runner
// replaying the owner's authorization with different arrays is refused.
TEST_F(UnitTest, PT_Publication_ConnectorRegistrationMustMatchItsSignedHash)
{
    ASSERT_TRUE(std::filesystem::exists(solcPath()));
    ASSERT_TRUE(std::filesystem::exists(ptPath() / "contracts"));

    PublicationFixture fixture("connector_registration_hash");

    TransformationRecord dependency;
    dependency.set_owner(chain::addressToHex(fixture.owner));
    dependency.mutable_transformation()->set_name("BoundTransform");
    dependency.mutable_transformation()->set_sol_src("return x;");

    const auto dependency_prepared = runAwaitable(
        fixture.io_context,
        loader::prepareTransformationPublication(*fixture.evm_instance, dependency, fixture.storage_path));
    ASSERT_TRUE(dependency_prepared.has_value());
    const auto dependency_address = fixture.publishTransformation("BoundTransform", fixture.owner);
    ASSERT_TRUE(dependency_address.has_value());

    registry::EntitySource dependency_source;
    dependency_source.content_hash = dependency_prepared->content_hash;
    ASSERT_TRUE(runAwaitable(
        fixture.io_context,
        fixture.registry.addTransformationVersion(dependency_source, *dependency_address, dependency)));

    ConnectorRecord connector;
    connector.set_owner(chain::addressToHex(fixture.owner));
    connector.mutable_connector()->set_name("BoundConnector");
    connector.mutable_connector()->add_dimensions()->add_transformations()->set_name("BoundTransform");

    auto prepared = runAwaitable(
        fixture.io_context,
        loader::prepareConnectorPublication(
            *fixture.evm_instance,
            fixture.registry,
            connector,
            fixture.storage_path,
            fixture.evm_instance->getRegistryAddress()));
    ASSERT_TRUE(prepared.has_value());
    ASSERT_TRUE(prepared->registration.has_value());

    pt::Publication & publication = prepared->publication;
    publication.nonce = 1;   // consumed once by the dependency

    pt::ConnectorRegistration forged = *prepared->registration;
    forged.transformation_names = {"SomethingElse"};

    const auto result = fixture.callRegistry(
        fixture.owner,
        pt::encodePublishConnectorCall(
            std::span<const std::uint8_t>(prepared->creation_code.data(), prepared->creation_code.size()),
            publication,
            forged,
            {}));
    EXPECT_TRUE(reverted(result));

    // The authorized arrays go through.
    const auto accepted = fixture.callRegistry(
        fixture.owner,
        pt::encodePublishConnectorCall(
            std::span<const std::uint8_t>(prepared->creation_code.data(), prepared->creation_code.size()),
            publication,
            *prepared->registration,
            {}));
    EXPECT_TRUE(accepted.has_value());
}

