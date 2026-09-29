#include "unit-tests.hpp"

#include "events_test_harness.hpp"

#include <algorithm>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "fake_eth_node.hpp"

using namespace dcn;
using namespace dcn::tests;
using namespace dcn::tests::events_harness;
using namespace dcn::tests::fake_eth;

namespace
{
    const std::string OPERATOR_KEY =
        "0x4646464646464646464646464646464646464646464646464646464646464646";

    std::shared_ptr<publish::Signer> makeSigner()
    {
        const std::filesystem::path key_path = buildPath() / "publish_tests_operator.key";
        std::ofstream out(key_path, std::ios::trunc);
        out << OPERATOR_KEY;
        out.close();
        // Key files readable by anyone but their owner are refused.
        std::filesystem::permissions(
            key_path,
            std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
            std::filesystem::perm_options::replace);

        auto signer = publish::Signer::fromKeyFile(key_path);
        if(!signer) throw std::runtime_error(signer.error());
        return *signer;
    }

    // An artifact store whose publish() writes to one durable replica.
    struct ArtifactDirs
    {
        TempEventsPaths paths;
        pt::ArtifactStore store;

        explicit ArtifactDirs(const std::string & test_name)
            : paths(makeTempEventsPaths(test_name))
            , store(paths.root / "cache", {}, {paths.root / "replica"})
        {
        }
    };

    std::string makeArtifactBytes(const pt::EntityKind kind, const std::string & name)
    {
        const auto bytes = pt::encodeArtifactBundle(pt::ArtifactBundle{
            .kind = kind,
            .name = name,
            .definition = {{"owner", hexAddress(0x42)}, {"name", name}},
            // Only transformations and conditions carry a user source fragment.
            .sol_src = kind == pt::EntityKind::CONNECTOR ? "" : "return x;",
            .generated_solidity = "contract Published {}",
            .pt_revision = chain::bytes32ToHex(chain::keccakString("pt sources")),
            .solc_version = "0.8.30",
            .creation_code_hash = chain::bytes32ToHex(bytes32OfFill(0xCC)),
            .runtime_code_hash = chain::bytes32ToHex(bytes32OfFill(0xDD)),
            .metadata_hash = chain::bytes32ToHex(pt::computeMetadataHash(kind, name))});
        if(!bytes) throw std::runtime_error(bytes.error());
        return *bytes;
    }

    publish::PublicationRequest makeTransformationRequest(const chain::Address & owner)
    {
        publish::PublicationRequest request;
        request.artifact_bytes = makeArtifactBytes(pt::EntityKind::TRANSFORMATION, "published_tx");
        request.publication.kind = pt::EntityKind::TRANSFORMATION;
        request.publication.name = "published_tx";
        request.publication.owner = owner;
        request.publication.content_hash = pt::computeContentHash(request.artifact_bytes);
        request.publication.metadata_hash = bytes32OfFill(0xBB);
        request.publication.creation_code_hash = bytes32OfFill(0xCC);
        request.publication.runtime_code_hash = bytes32OfFill(0xDD);
        request.creation_code = std::vector<std::uint8_t>{0x60, 0x00, 0x60, 0x00, 0xF3};
        return request;
    }

    publish::PublicationRequest makeConnectorRequest(const chain::Address & owner)
    {
        publish::PublicationRequest request;
        request.artifact_bytes = makeArtifactBytes(pt::EntityKind::CONNECTOR, "published_connector");
        request.publication.kind = pt::EntityKind::CONNECTOR;
        request.publication.name = "published_connector";
        request.publication.owner = owner;
        request.publication.content_hash = pt::computeContentHash(request.artifact_bytes);
        request.publication.metadata_hash = bytes32OfFill(0xBB);
        request.publication.creation_code_hash = bytes32OfFill(0xCC);
        request.publication.runtime_code_hash = bytes32OfFill(0xDD);
        request.creation_code = std::vector<std::uint8_t>{0x60, 0x00, 0x60, 0x00, 0xF3};
        request.registration = pt::ConnectorRegistration{
            .dimensions_count = 1,
            .transformation_dim_ids = {0},
            .transformation_names = {"dep_tx"},
            .transformation_arg_counts = {0}};
        return request;
    }

    publish::PublicationClaim claimFor(const chain::Address & owner)
    {
        return publish::PublicationClaim{
            .kind = pt::EntityKind::TRANSFORMATION,
            .name = "published_tx",
            .owner = owner,
            .content_hash = chain::bytes32ToHex(bytes32OfFill(0xAA)),
            .metadata_hash = chain::bytes32ToHex(bytes32OfFill(0xBB))};
    }

    publish::Transaction sampleTransaction()
    {
        return publish::Transaction{
            .chain_id = 11155111,
            .nonce = 7,
            .max_priority_fee_per_gas = 1'500'000'000,
            .max_fee_per_gas = 30'000'000'000,
            .gas_limit = 210'000,
            .to = evmc::from_hex<chain::Address>(REGISTRY_ADDRESS).value(),
            .value = 0,
            .data = std::vector<std::uint8_t>{0xAB, 0xCD}
        };
    }

    std::vector<std::uint8_t> withoutLeadingZeros(const std::span<const std::uint8_t> word)
    {
        const auto first = std::ranges::find_if(word, [](const std::uint8_t b) { return b != 0; });
        return std::vector<std::uint8_t>(first, word.end());
    }

    // The twelve RLP items of a signed type-2 transaction, to build variants from.
    std::vector<std::vector<std::uint8_t>> signedFields(
        const publish::Transaction & transaction,
        const std::span<const std::uint8_t> signature)
    {
        return {
            publish::rlp::encodeUint(transaction.chain_id),
            publish::rlp::encodeUint(transaction.nonce),
            publish::rlp::encodeUint(transaction.max_priority_fee_per_gas),
            publish::rlp::encodeUint(transaction.max_fee_per_gas),
            publish::rlp::encodeUint(transaction.gas_limit),
            publish::rlp::encodeBytes(std::span<const std::uint8_t>(transaction.to->bytes, 20)),
            publish::rlp::encodeUint(transaction.value),
            publish::rlp::encodeBytes(transaction.data),
            publish::rlp::encodeList({}),
            publish::rlp::encodeUint(signature[64] - 27u),
            publish::rlp::encodeBytes(withoutLeadingZeros(signature.subspan(0, 32))),
            publish::rlp::encodeBytes(withoutLeadingZeros(signature.subspan(32, 32)))};
    }

    std::string rawOf(const std::vector<std::vector<std::uint8_t>> & fields)
    {
        std::vector<std::uint8_t> raw{0x02};
        const std::vector<std::uint8_t> body = publish::rlp::encodeList(fields);
        raw.insert(raw.end(), body.begin(), body.end());
        return chain::bytesToHex(raw);
    }

    // n - s for the secp256k1 group order n: the other valid `s` of the same signature.
    std::vector<std::uint8_t> negateScalar(const std::span<const std::uint8_t> scalar)
    {
        const auto order =
            evmc::from_hex("0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEBAAEDCE6AF48A03BBFD25E8CD0364141").value();
        std::vector<std::uint8_t> out(32, 0);
        int borrow = 0;
        for(int i = 31; i >= 0; --i)
        {
            int difference = static_cast<int>(order[i]) - static_cast<int>(scalar[i]) - borrow;
            borrow = difference < 0 ? 1 : 0;
            if(difference < 0) difference += 256;
            out[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(difference);
        }
        return out;
    }

    publish::PublicationClaim relayClaim(const pt::Publication & publication)
    {
        return publish::PublicationClaim{
            .kind = publication.kind,
            .name = publication.name,
            .owner = publication.owner,
            .content_hash = chain::bytes32ToHex(publication.content_hash),
            .metadata_hash = chain::bytes32ToHex(publication.metadata_hash)};
    }

    // Four contracts' worth of compiler output for a bootstrap.
    publish::BootstrapArtifacts writeBootstrapArtifacts(const std::filesystem::path & dir)
    {
        std::filesystem::create_directories(dir);
        const publish::BootstrapArtifacts artifacts{
            .registry_impl_bin = dir / "RegistryBase.bin",
            .registry_proxy_bin = dir / "PTRegistryProxy.bin",
            .runner_impl_bin = dir / "Runner.bin",
            .runner_proxy_bin = dir / "PTContractProxy.bin"};
        for(const auto & path : {
            artifacts.registry_impl_bin, artifacts.registry_proxy_bin,
            artifacts.runner_impl_bin, artifacts.runner_proxy_bin})
        {
            std::ofstream(path, std::ios::trunc) << "6000600055\n";
        }
        return artifacts;
    }
}

// ---------------------------------------------------------------------------
// RLP and transaction signing
// ---------------------------------------------------------------------------

TEST(PublishEthTx, RlpMatchesSpecVectors)
{
    const auto bytesOf = [](const std::string & text)
    {
        return std::vector<std::uint8_t>(text.begin(), text.end());
    };

    const auto dog = bytesOf("dog");
    EXPECT_EQ(publish::rlp::encodeBytes(dog), (std::vector<std::uint8_t>{0x83, 'd', 'o', 'g'}));
    EXPECT_EQ(publish::rlp::encodeBytes({}), (std::vector<std::uint8_t>{0x80}));

    // RLP integers are minimal big-endian and zero is the empty string.
    EXPECT_EQ(publish::rlp::encodeUint(0), (std::vector<std::uint8_t>{0x80}));
    EXPECT_EQ(publish::rlp::encodeUint(15), (std::vector<std::uint8_t>{0x0F}));
    EXPECT_EQ(publish::rlp::encodeUint(1024), (std::vector<std::uint8_t>{0x82, 0x04, 0x00}));

    EXPECT_EQ(publish::rlp::encodeList({}), (std::vector<std::uint8_t>{0xC0}));

    const std::vector<std::vector<std::uint8_t>> cat_dog{
        publish::rlp::encodeBytes(bytesOf("cat")),
        publish::rlp::encodeBytes(bytesOf("dog"))};
    EXPECT_EQ(
        publish::rlp::encodeList(cat_dog),
        (std::vector<std::uint8_t>{0xC8, 0x83, 'c', 'a', 't', 0x83, 'd', 'o', 'g'}));

    // A 56-byte payload crosses into the long-form length prefix.
    const std::vector<std::uint8_t> long_payload(56, 0x7F);
    const auto encoded_long = publish::rlp::encodeBytes(long_payload);
    EXPECT_EQ(encoded_long.at(0), 0xB8);
    EXPECT_EQ(encoded_long.at(1), 56);
    EXPECT_EQ(encoded_long.size(), 58u);
}

TEST(PublishSigner, DerivesAddressAndProducesRecoverableSignatures)
{
    const auto signer = makeSigner();

    const evmc::bytes32 digest = chain::keccakString("publication digest");
    const auto signature = signer->signDigest(digest);
    ASSERT_TRUE(signature.has_value());
    ASSERT_EQ(signature->size(), 65u);
    EXPECT_TRUE(signature->at(64) == 27 || signature->at(64) == 28);

    const auto recovered = pt::recoverSigner(digest, *signature);
    ASSERT_TRUE(recovered.has_value());
    EXPECT_EQ(*recovered, signer->address());
}

TEST(PublishSigner, RejectsMalformedKeyMaterial)
{
    const std::filesystem::path key_path = buildPath() / "publish_tests_bad.key";
    std::ofstream out(key_path, std::ios::trunc);
    out << "0xdeadbeef";
    out.close();

    EXPECT_FALSE(publish::Signer::fromKeyFile(key_path).has_value());
    EXPECT_FALSE(publish::Signer::fromKeyFile(buildPath() / "publish_tests_missing.key").has_value());
}

TEST(PublishEthTx, SignedTransactionRecoversToTheSigner)
{
    const auto signer = makeSigner();

    const publish::Transaction transaction{
        .chain_id = 11155111,
        .nonce = 7,
        .max_priority_fee_per_gas = 1'500'000'000,
        .max_fee_per_gas = 30'000'000'000,
        .gas_limit = 210'000,
        .to = evmc::from_hex<chain::Address>(REGISTRY_ADDRESS).value(),
        .value = 0,
        .data = std::vector<std::uint8_t>{0xAB, 0xCD}
    };

    const evmc::bytes32 hash = publish::signingHash(transaction);
    const auto signature = signer->signDigest(hash);
    ASSERT_TRUE(signature.has_value());

    const auto recovered = pt::recoverSigner(hash, *signature);
    ASSERT_TRUE(recovered.has_value());
    EXPECT_EQ(*recovered, signer->address());

    const std::string raw = publish::encodeSignedTransaction(transaction, *signature);
    ASSERT_FALSE(raw.empty());
    // Type-2 envelope, followed by the RLP list of twelve fields.
    EXPECT_TRUE(raw.starts_with("0x02"));
    EXPECT_EQ(raw.substr(4, 1), "f");
    EXPECT_EQ(publish::transactionHash(raw).size(), 66u);

    // A different nonce must produce a different signing hash and a different raw form.
    publish::Transaction other = transaction;
    other.nonce = 8;
    EXPECT_NE(publish::signingHash(other), hash);
}

TEST(PublishEthTx, DecodesASignedTransactionAndRecoversItsSender)
{
    const auto signer = makeSigner();
    const publish::Transaction transaction = sampleTransaction();
    const std::string raw = signer->signTransaction(transaction).value();

    const auto decoded = publish::decodeSignedTransaction(raw);
    ASSERT_TRUE(decoded.has_value()) << (decoded ? "" : decoded.error());
    EXPECT_EQ(decoded->sender, signer->address());
    EXPECT_EQ(decoded->tx_hash, publish::transactionHash(raw));
    EXPECT_EQ(decoded->transaction.chain_id, transaction.chain_id);
    EXPECT_EQ(decoded->transaction.nonce, transaction.nonce);
    EXPECT_EQ(decoded->transaction.max_priority_fee_per_gas, transaction.max_priority_fee_per_gas);
    EXPECT_EQ(decoded->transaction.max_fee_per_gas, transaction.max_fee_per_gas);
    EXPECT_EQ(decoded->transaction.gas_limit, transaction.gas_limit);
    EXPECT_EQ(decoded->transaction.to, transaction.to);
    EXPECT_EQ(decoded->transaction.value, transaction.value);
    EXPECT_EQ(decoded->transaction.data, transaction.data);
}

TEST(PublishEthTx, RefusesEveryFormButTheCanonicalSignedTransaction)
{
    const auto signer = makeSigner();
    const publish::Transaction transaction = sampleTransaction();
    const std::vector<std::uint8_t> signature = signer->signDigest(publish::signingHash(transaction)).value();
    const std::vector<std::vector<std::uint8_t>> fields = signedFields(transaction, signature);
    const std::string raw = rawOf(fields);
    ASSERT_EQ(raw, signer->signTransaction(transaction).value());
    ASSERT_TRUE(publish::decodeSignedTransaction(raw).has_value());

    const auto refused = [](const std::string & candidate)
    {
        return !publish::decodeSignedTransaction(candidate).has_value();
    };
    const auto variant = [&](const std::size_t index, std::vector<std::uint8_t> item)
    {
        std::vector<std::vector<std::uint8_t>> copy = fields;
        copy[index] = std::move(item);
        return rawOf(copy);
    };

    EXPECT_TRUE(refused(""));
    EXPECT_TRUE(refused("0xzz"));
    EXPECT_TRUE(refused("0x01" + raw.substr(4)));             // another envelope type
    EXPECT_TRUE(refused(raw + "00"));                         // trailing byte
    EXPECT_TRUE(refused(raw.substr(0, raw.size() - 2)));      // truncated

    // An integer with a leading zero byte is a second encoding of the same value.
    const std::vector<std::uint8_t> padded_nonce{0x00, 0x07};
    EXPECT_TRUE(refused(variant(1, publish::rlp::encodeBytes(padded_nonce))));

    // A quantity wider than 64 bits.
    const std::vector<std::uint8_t> wide(9, 0x01);
    EXPECT_TRUE(refused(variant(0, publish::rlp::encodeBytes(wide))));

    // A contract creation has no recipient.
    EXPECT_TRUE(refused(variant(5, publish::rlp::encodeBytes({}))));

    // A non-empty access list.
    const std::vector<std::vector<std::uint8_t>> entry{
        publish::rlp::encodeBytes(std::vector<std::uint8_t>(20, 0x11)),
        publish::rlp::encodeList({})};
    const std::vector<std::vector<std::uint8_t>> access_list{publish::rlp::encodeList(entry)};
    EXPECT_TRUE(refused(variant(8, publish::rlp::encodeList(access_list))));

    // yParity is 0 or 1.
    EXPECT_TRUE(refused(variant(9, publish::rlp::encodeUint(2))));

    // Eleven fields.
    std::vector<std::vector<std::uint8_t>> short_fields = fields;
    short_fields.pop_back();
    EXPECT_TRUE(refused(rawOf(short_fields)));

    // The high-s twin of the signature recovers the same key but would give the same
    // transaction a second hash.
    std::vector<std::uint8_t> twin = signature;
    const std::vector<std::uint8_t> high_s = negateScalar(std::span<const std::uint8_t>(signature).subspan(32, 32));
    std::ranges::copy(high_s, twin.begin() + 32);
    twin[64] = signature[64] == 27 ? 28 : 27;
    EXPECT_TRUE(refused(publish::encodeSignedTransaction(transaction, twin)));
}

TEST(PublishPublication, DecodesThePublicationOfEveryPublishCall)
{
    const publish::PublicationRequest connector = makeConnectorRequest(makeAddressFromByte(0x42));
    pt::Publication publication = connector.publication;
    publication.registration_hash = bytes32OfFill(0x01);
    publication.nonce = 3;
    publication.deadline = 1'900'000'000;

    const auto encode = [&](const pt::Publication & target)
    {
        switch(target.kind)
        {
        case pt::EntityKind::TRANSFORMATION:
            return pt::encodePublishTransformationCall(connector.creation_code, target, {});
        case pt::EntityKind::CONDITION:
            return pt::encodePublishConditionCall(connector.creation_code, target, {});
        case pt::EntityKind::CONNECTOR:
            break;
        }
        return pt::encodePublishConnectorCall(connector.creation_code, target, *connector.registration, {});
    };

    for(const pt::EntityKind kind : {pt::EntityKind::TRANSFORMATION, pt::EntityKind::CONDITION, pt::EntityKind::CONNECTOR})
    {
        pt::Publication expected = publication;
        expected.kind = kind;
        const auto decoded = pt::decodePublishCall(encode(expected));
        ASSERT_TRUE(decoded.has_value()) << pt::entityKindName(kind);
        EXPECT_EQ(decoded->kind, kind);
        EXPECT_EQ(decoded->name, expected.name);
        EXPECT_EQ(decoded->owner, expected.owner);
        EXPECT_EQ(decoded->content_hash, expected.content_hash);
        EXPECT_EQ(decoded->metadata_hash, expected.metadata_hash);
        EXPECT_EQ(decoded->creation_code_hash, expected.creation_code_hash);
        EXPECT_EQ(decoded->runtime_code_hash, expected.runtime_code_hash);
        EXPECT_EQ(decoded->registration_hash, expected.registration_hash);
        EXPECT_EQ(decoded->nonce, expected.nonce);
        EXPECT_EQ(decoded->deadline, expected.deadline);
    }

    // Any other registry call.
    EXPECT_FALSE(pt::decodePublishCall(pt::encodePublicationNonceCall(publication.owner)).has_value());

    // A publication tuple whose kind disagrees with the called function.
    pt::Publication condition = publication;
    condition.kind = pt::EntityKind::CONDITION;
    EXPECT_FALSE(pt::decodePublishCall(
        pt::encodePublishTransformationCall(connector.creation_code, condition, {})).has_value());

    // Truncated calldata.
    pt::Publication transformation = publication;
    transformation.kind = pt::EntityKind::TRANSFORMATION;
    std::vector<std::uint8_t> truncated = encode(transformation);
    truncated.resize(4 + 64);
    EXPECT_FALSE(pt::decodePublishCall(truncated).has_value());
    EXPECT_FALSE(pt::decodePublishCall(std::vector<std::uint8_t>{0x01, 0x02}).has_value());
}

TEST(PublishPublication, DigestBindsChainRegistryAndFields)
{
    const chain::Address owner = makeAddressFromByte(0x42);
    pt::Publication publication;
    publication.kind = pt::EntityKind::TRANSFORMATION;
    publication.name = "bound";
    publication.owner = owner;
    publication.content_hash = bytes32OfFill(0xAA);
    publication.metadata_hash = bytes32OfFill(0xBB);
    publication.creation_code_hash = bytes32OfFill(0xCC);
    publication.runtime_code_hash = bytes32OfFill(0xDD);
    publication.nonce = 1;
    publication.deadline = 1'900'000'000;

    const chain::Address registry = evmc::from_hex<chain::Address>(REGISTRY_ADDRESS).value();
    const chain::Address other_registry = makeAddressFromByte(0xBA);

    const evmc::bytes32 base = pt::publicationDigest(1, registry, publication);
    EXPECT_NE(base, pt::publicationDigest(2, registry, publication));
    EXPECT_NE(base, pt::publicationDigest(1, other_registry, publication));

    pt::Publication renamed = publication;
    renamed.name = "bound2";
    EXPECT_NE(base, pt::publicationDigest(1, registry, renamed));

    pt::Publication replayed = publication;
    replayed.nonce = 2;
    EXPECT_NE(base, pt::publicationDigest(1, registry, replayed));

    // The connector arrays the event will carry are part of what the owner authorizes.
    pt::Publication registered = publication;
    registered.registration_hash = bytes32OfFill(0x01);
    EXPECT_NE(base, pt::publicationDigest(1, registry, registered));

    // Stable for identical input: the client and the registry must agree byte for byte.
    EXPECT_EQ(base, pt::publicationDigest(1, registry, publication));
}

TEST(PublishPublication, EntityAddressDependsOnEveryBoundField)
{
    const chain::Address registry = evmc::from_hex<chain::Address>(REGISTRY_ADDRESS).value();
    const publish::PublicationRequest request = makeTransformationRequest(makeAddressFromByte(0x42));

    const chain::Address address = pt::predictEntityAddress(registry, request.publication, request.creation_code);
    EXPECT_NE(address, chain::Address{});
    EXPECT_EQ(address, pt::predictEntityAddress(registry, request.publication, request.creation_code));

    pt::Publication renamed = request.publication;
    renamed.name = "other";
    EXPECT_NE(address, pt::predictEntityAddress(registry, renamed, request.creation_code));

    // A cleared name published again with the same content lands at a fresh address.
    pt::Publication republished = request.publication;
    republished.nonce = request.publication.nonce + 1;
    EXPECT_NE(address, pt::predictEntityAddress(registry, republished, request.creation_code));

    std::vector<std::uint8_t> other_code = request.creation_code;
    other_code.push_back(0x00);
    EXPECT_NE(address, pt::predictEntityAddress(registry, request.publication, other_code));

    EXPECT_NE(address, pt::predictEntityAddress(makeAddressFromByte(0xBA), request.publication, request.creation_code));
}

TEST(PublishPublication, DescribesARevertReason)
{
    std::vector<std::uint8_t> error = chain::constructSelector("Error(string)");
    const auto reason = pt::abi::encodeTuple({pt::abi::stringComponent("name taken")});
    error.insert(error.end(), reason.begin(), reason.end());

    EXPECT_EQ(publish::describeRevert(chain::bytesToHex(error)), "transaction reverted: name taken");
    EXPECT_EQ(publish::describeRevert("0x"), "transaction reverted");
    EXPECT_EQ(publish::describeRevert("0x12345678"), "transaction reverted: 0x12345678");
}

// ---------------------------------------------------------------------------
// Deployment manifest
// ---------------------------------------------------------------------------

TEST(PublishManifest, RoundTripsThroughDisk)
{
    const std::filesystem::path path = buildPath() / "publish_manifest_roundtrip.json";
    std::error_code ec;
    std::filesystem::remove(path, ec);

    const publish::DeploymentManifest manifest = makeManifest();
    ASSERT_TRUE(publish::writeManifest(path, manifest).has_value());

    const auto loaded = publish::readManifest(path);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->chain_id, manifest.chain_id);
    EXPECT_EQ(loaded->registry, manifest.registry);
    EXPECT_EQ(loaded->runner, manifest.runner);
    EXPECT_EQ(loaded->registry_impl, manifest.registry_impl);
    EXPECT_EQ(loaded->registry_impl_code_hash, manifest.registry_impl_code_hash);
    EXPECT_EQ(loaded->deployment_block, manifest.deployment_block);
    EXPECT_EQ(loaded->protocol_version, manifest.protocol_version);
    EXPECT_EQ(loaded->owner, manifest.owner);
}

TEST(PublishManifest, RejectsIncompleteOrZeroFields)
{
    const std::filesystem::path path = buildPath() / "publish_manifest_invalid.json";

    publish::DeploymentManifest zero_owner = makeManifest();
    zero_owner.owner = chain::addressToHex(chain::Address{});
    ASSERT_TRUE(publish::writeManifest(path, zero_owner).has_value());
    EXPECT_FALSE(publish::readManifest(path).has_value());

    publish::DeploymentManifest no_chain = makeManifest();
    no_chain.chain_id = 0;
    ASSERT_TRUE(publish::writeManifest(path, no_chain).has_value());
    EXPECT_FALSE(publish::readManifest(path).has_value());

    publish::DeploymentManifest bad_hash = makeManifest();
    bad_hash.registry_impl_code_hash = "0x1234";
    ASSERT_TRUE(publish::writeManifest(path, bad_hash).has_value());
    EXPECT_FALSE(publish::readManifest(path).has_value());

    EXPECT_FALSE(publish::readManifest(buildPath() / "publish_manifest_missing.json").has_value());
}

TEST(PublishManifest, VerifiesLiveDeploymentAndRejectsDrift)
{
    asio::io_context io_context;
    auto node = std::make_shared<FakeNode>();
    node->implementation_by_proxy[REGISTRY_ADDRESS] = wordOfAddress(REGISTRY_IMPL_ADDRESS);
    node->implementation_by_proxy[RUNNER_ADDRESS] = wordOfAddress(RUNNER_IMPL_ADDRESS);

    publish::DeploymentManifest manifest = makeManifest();
    // The fake node reports the same code for every address, so pin the manifest hashes to it.
    const auto code_bytes = evmc::from_hex(node->code).value();
    const std::string code_hash =
        chain::bytes32ToHex(chain::keccakBytes(code_bytes.data(), code_bytes.size()));
    manifest.registry_impl_code_hash = code_hash;
    manifest.runner_impl_code_hash = code_hash;

    const auto verified = runAwaitable(io_context, publish::verifyDeployment(*makeClient(node), manifest));
    EXPECT_TRUE(verified.has_value()) << (verified ? "" : verified.error());

    // A proxy pointing somewhere the manifest does not pin must be refused.
    node->implementation_by_proxy[REGISTRY_ADDRESS] = wordOfAddress(hexAddress(0x01));
    EXPECT_FALSE(runAwaitable(io_context, publish::verifyDeployment(*makeClient(node), manifest)).has_value());

    // So must an upgrade owner other than the pinned one.
    node->implementation_by_proxy[REGISTRY_ADDRESS] = wordOfAddress(REGISTRY_IMPL_ADDRESS);
    node->owner_word = wordOfAddress(hexAddress(0x78));
    EXPECT_FALSE(runAwaitable(io_context, publish::verifyDeployment(*makeClient(node), manifest)).has_value());

    // So must a runner linked to another registry.
    node->owner_word = wordOfAddress(hexAddress(0x77));
    node->runner_registry_word = wordOfAddress(hexAddress(0x02));
    EXPECT_FALSE(runAwaitable(io_context, publish::verifyDeployment(*makeClient(node), manifest)).has_value());
}

TEST(PublishManifest, VerifiesAnAddressOnlyRunner)
{
    asio::io_context io_context;
    auto node = std::make_shared<FakeNode>();

    EXPECT_TRUE(runAwaitable(
        io_context, rpc::verifyPtRunner(*makeClient(node), RUNNER_ADDRESS, REGISTRY_ADDRESS)).has_value());

    node->runner_registry_word = wordOfAddress(hexAddress(0x02));
    EXPECT_FALSE(runAwaitable(
        io_context, rpc::verifyPtRunner(*makeClient(node), RUNNER_ADDRESS, REGISTRY_ADDRESS)).has_value());
}

// ---------------------------------------------------------------------------
// Publisher: preparation
// ---------------------------------------------------------------------------

TEST(PublishPublisher, PreparesAnUnsignedTransactionTheOwnerSends)
{
    asio::io_context io_context;
    auto node = std::make_shared<FakeNode>();
    ArtifactDirs artifacts("publish_prepare");
    publish::Publisher publisher(makeClient(node), makeManifest(), artifacts.store);

    const chain::Address owner = makeAddressFromByte(0x42);
    const publish::PublicationRequest request = makeTransformationRequest(owner);

    const auto prepared = runAwaitable(io_context, publisher.prepare(request));
    ASSERT_TRUE(prepared.has_value()) << (prepared ? "" : prepared.error().message);

    EXPECT_EQ(prepared->from, chain::addressToHex(owner));
    EXPECT_EQ(prepared->to, REGISTRY_ADDRESS);
    EXPECT_EQ(prepared->chain_id, CHAIN_ID);
    EXPECT_EQ(prepared->gas, 180'000u);   // estimate plus 20% headroom
    EXPECT_EQ(prepared->publication_nonce, 3u);
    EXPECT_EQ(prepared->content_hash, chain::bytes32ToHex(request.publication.content_hash));
    EXPECT_GT(prepared->deadline, 1'700'000'000u);

    // The calldata is the factory call for exactly this publication with no signature:
    // the registry accepts that because the owner sends it.
    pt::Publication expected = request.publication;
    expected.nonce = 3;
    expected.deadline = prepared->deadline;
    EXPECT_EQ(prepared->data, chain::bytesToHex(
        pt::encodePublishTransformationCall(request.creation_code, expected, {})));
    EXPECT_EQ(prepared->entity_address, chain::addressToHex(pt::predictEntityAddress(
        evmc::from_hex<chain::Address>(REGISTRY_ADDRESS).value(), expected, request.creation_code)));

    // The server never sends anything itself, and the artifact is durable before the
    // owner can broadcast.
    EXPECT_TRUE(node->sent_raw_transactions.empty());
    EXPECT_EQ(artifacts.store.read(prepared->content_hash).state, pt::ArtifactState::AVAILABLE);
}

TEST(PublishPublisher, ReportsADeterministicRevertBeforeTheOwnerPays)
{
    asio::io_context io_context;
    auto node = std::make_shared<FakeNode>();
    node->estimate_error = rpc::RpcError{
        .kind = rpc::RpcError::Kind::EXECUTION,
        .code = 3,
        .message = "execution reverted"};
    ArtifactDirs artifacts("publish_prepare_revert");
    publish::Publisher publisher(makeClient(node), makeManifest(), artifacts.store);

    const auto result = runAwaitable(
        io_context, publisher.prepare(makeTransformationRequest(makeAddressFromByte(0x42))));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, publish::PublishError::Kind::REVERTED);
}

TEST(PublishPublisher, RefusesAnArtifactThatDoesNotMatchTheContentHash)
{
    asio::io_context io_context;
    auto node = std::make_shared<FakeNode>();
    ArtifactDirs artifacts("publish_prepare_bad_artifact");
    publish::Publisher publisher(makeClient(node), makeManifest(), artifacts.store);

    publish::PublicationRequest request = makeTransformationRequest(makeAddressFromByte(0x42));
    request.publication.content_hash = bytes32OfFill(0x01);

    const auto result = runAwaitable(io_context, publisher.prepare(request));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, publish::PublishError::Kind::INVALID);
}

TEST(PublishPublisher, ConnectorDependenciesMustBeTheArtifactsOnTheTargetRegistry)
{
    asio::io_context io_context;
    auto node = std::make_shared<FakeNode>();
    node->content_hash_by_name["dep_tx"] = wordOfFill(0x00);        // not registered
    node->content_hash_by_name["dep_cond"] = wordOfFill(0x11);      // registered, other content
    node->content_hash_by_name["dep_ok"] = wordOfFill(0x22);
    ArtifactDirs artifacts("publish_prepare_dependencies");
    publish::Publisher publisher(makeClient(node), makeManifest(), artifacts.store);

    publish::PublicationRequest request = makeConnectorRequest(makeAddressFromByte(0x42));
    request.dependencies = {
        {.kind = pt::EntityKind::TRANSFORMATION, .name = "dep_tx", .content_hash = wordOfFill(0x33)},
        {.kind = pt::EntityKind::CONDITION, .name = "dep_cond", .content_hash = wordOfFill(0x44)},
        {.kind = pt::EntityKind::TRANSFORMATION, .name = "dep_ok", .content_hash = wordOfFill(0x22)}};

    const auto result = runAwaitable(io_context, publisher.prepare(request));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, publish::PublishError::Kind::CONFLICT);
    EXPECT_EQ(result.error().missing, (std::vector<std::string>{"dep_tx"}));
    EXPECT_EQ(result.error().mismatched, (std::vector<std::string>{"dep_cond"}));

    // With every dependency in place the connector publication binds its registration.
    node->content_hash_by_name["dep_tx"] = wordOfFill(0x33);
    node->content_hash_by_name["dep_cond"] = wordOfFill(0x44);
    const auto prepared = runAwaitable(io_context, publisher.prepare(request));
    ASSERT_TRUE(prepared.has_value()) << (prepared ? "" : prepared.error().message);

    pt::Publication expected = request.publication;
    expected.nonce = 3;
    expected.deadline = prepared->deadline;
    expected.registration_hash = pt::computeRegistrationHash(*request.registration);
    EXPECT_EQ(prepared->data, chain::bytesToHex(
        pt::encodePublishConnectorCall(request.creation_code, expected, *request.registration, {})));
}

TEST(PublishPublisher, FindsAnIdenticalPublicationAndRefusesAForeignOne)
{
    asio::io_context io_context;
    auto node = std::make_shared<FakeNode>();
    ArtifactDirs artifacts("publish_find");
    publish::Publisher publisher(makeClient(node), makeManifest(), artifacts.store);

    publish::PublicationRequest request = makeTransformationRequest(makeAddressFromByte(0x42));
    request.publication.content_hash = bytes32OfFill(0xAA);

    const auto unregistered = runAwaitable(io_context, publisher.findPublished(request.publication));
    ASSERT_TRUE(unregistered.has_value());
    EXPECT_FALSE(unregistered->has_value());

    node->contains_entity = true;
    const auto existing = runAwaitable(io_context, publisher.findPublished(request.publication));
    ASSERT_TRUE(existing.has_value());
    ASSERT_TRUE(existing->has_value());
    EXPECT_TRUE((*existing)->already_published);
    EXPECT_EQ((*existing)->entity_address, hexAddress(0x55));
    EXPECT_EQ((*existing)->owner, hexAddress(0x42));

    node->entity_content_hash = wordOfFill(0x11);
    const auto foreign = runAwaitable(io_context, publisher.findPublished(request.publication));
    ASSERT_FALSE(foreign.has_value());
    EXPECT_EQ(foreign.error().kind, publish::PublishError::Kind::CONFLICT);
}

// ---------------------------------------------------------------------------
// Publisher: confirmation
// ---------------------------------------------------------------------------

TEST(PublishPublisher, ConfirmsTheRegistryEventOfTheOwnersTransaction)
{
    asio::io_context io_context;
    auto node = std::make_shared<FakeNode>();
    ArtifactDirs artifacts("publish_confirm");
    publish::Publisher publisher(makeClient(node), makeManifest(), artifacts.store);

    const chain::Address owner = makeAddressFromByte(0x42);
    const std::string tx_hash = hexBytes(0x99, 32);
    node->receipt = makeTransformationAddedReceipt(tx_hash, "published_tx", owner, bytes32OfFill(0xAA), bytes32OfFill(0xBB));

    const auto result = runAwaitable(io_context, publisher.confirm(claimFor(owner), tx_hash));
    ASSERT_TRUE(result.has_value()) << (result ? "" : result.error().message);
    EXPECT_EQ(result->tx_hash, tx_hash);
    EXPECT_EQ(result->block_number, 99);
    EXPECT_EQ(result->entity_address, hexAddress(0x55));
    EXPECT_EQ(result->owner, chain::addressToHex(owner));
}

TEST(PublishPublisher, ConfirmationRejectsEvidenceOfAnythingElse)
{
    asio::io_context io_context;
    auto node = std::make_shared<FakeNode>();
    ArtifactDirs artifacts("publish_confirm_wrong");
    publish::Publisher publisher(makeClient(node), makeManifest(), artifacts.store);

    const chain::Address owner = makeAddressFromByte(0x42);
    const std::string tx_hash = hexBytes(0x99, 32);

    // A log from another contract is not evidence that the registry registered anything.
    node->receipt = makeTransformationAddedReceipt(tx_hash, "published_tx", owner, bytes32OfFill(0xAA), bytes32OfFill(0xBB), hexAddress(0x01));
    const auto foreign_log = runAwaitable(io_context, publisher.confirm(claimFor(owner), tx_hash));
    ASSERT_FALSE(foreign_log.has_value());
    EXPECT_EQ(foreign_log.error().kind, publish::PublishError::Kind::INVALID);

    // Neither is an event whose hashes describe a different publication.
    node->receipt = makeTransformationAddedReceipt(tx_hash, "published_tx", owner, bytes32OfFill(0x01), bytes32OfFill(0x02));
    const auto wrong_hashes = runAwaitable(io_context, publisher.confirm(claimFor(owner), tx_hash));
    ASSERT_FALSE(wrong_hashes.has_value());
    EXPECT_EQ(wrong_hashes.error().kind, publish::PublishError::Kind::INVALID);

    // Nor one registering the name for someone else.
    node->receipt = makeTransformationAddedReceipt(tx_hash, "published_tx", makeAddressFromByte(0x43), bytes32OfFill(0xAA), bytes32OfFill(0xBB));
    const auto other_owner = runAwaitable(io_context, publisher.confirm(claimFor(owner), tx_hash));
    ASSERT_FALSE(other_owner.has_value());
    EXPECT_EQ(other_owner.error().kind, publish::PublishError::Kind::INVALID);

    const auto not_a_hash = runAwaitable(io_context, publisher.confirm(claimFor(owner), "0x1234"));
    ASSERT_FALSE(not_a_hash.has_value());
    EXPECT_EQ(not_a_hash.error().kind, publish::PublishError::Kind::INVALID);
}

TEST(PublishPublisher, ConfirmationReportsPendingAndRevertedTransactions)
{
    asio::io_context io_context;
    auto node = std::make_shared<FakeNode>();
    ArtifactDirs artifacts("publish_confirm_pending");
    publish::Publisher publisher(makeClient(node), makeManifest(), artifacts.store);

    const chain::Address owner = makeAddressFromByte(0x42);
    const std::string tx_hash = hexBytes(0x99, 32);

    // No receipt yet: the outcome is unknown, and asking again is the status check.
    const auto pending = runAwaitable(io_context, publisher.confirm(claimFor(owner), tx_hash));
    ASSERT_FALSE(pending.has_value());
    EXPECT_EQ(pending.error().kind, publish::PublishError::Kind::PENDING);
    EXPECT_EQ(pending.error().tx_hash, tx_hash);

    node->receipt = json{
        {"status", "0x0"},
        {"transactionHash", tx_hash},
        {"blockNumber", "0x63"},
        {"blockHash", hexBytes(0x41, 32)},
        {"gasUsed", "0x5208"},
        {"logs", json::array()}
    };
    const auto reverted = runAwaitable(io_context, publisher.confirm(claimFor(owner), tx_hash));
    ASSERT_FALSE(reverted.has_value());
    EXPECT_EQ(reverted.error().kind, publish::PublishError::Kind::REVERTED);
}

TEST(PublishPublisher, PreparesTheAccountNonceAndFeesOnlyForARelay)
{
    asio::io_context io_context;
    auto node = std::make_shared<FakeNode>();
    ArtifactDirs artifacts("publish_prepare_relay");
    publish::Publisher publisher(makeClient(node), makeManifest(), artifacts.store);
    const chain::Address owner = makeAddressFromByte(0x42);

    // A browser wallet chooses its own nonce and fees.
    const auto for_wallet = runAwaitable(io_context, publisher.prepare(makeTransformationRequest(owner)));
    ASSERT_TRUE(for_wallet.has_value()) << (for_wallet ? "" : for_wallet.error().message);
    EXPECT_FALSE(for_wallet->signing.has_value());

    const auto for_relay = runAwaitable(io_context, publisher.prepare(makeTransformationRequest(owner), true));
    ASSERT_TRUE(for_relay.has_value()) << (for_relay ? "" : for_relay.error().message);
    ASSERT_TRUE(for_relay->signing.has_value());
    EXPECT_EQ(for_relay->signing->nonce, node->tx_count);
    EXPECT_EQ(for_relay->signing->max_priority_fee_per_gas, node->priority_fee);
    EXPECT_EQ(for_relay->signing->max_fee_per_gas, node->base_fee * 2 + node->priority_fee);
    EXPECT_TRUE(node->sent_raw_transactions.empty());
}

TEST(PublishPublisher, SendsTheOwnersSignedPublicationThroughTheProvider)
{
    asio::io_context io_context;
    auto node = std::make_shared<FakeNode>();
    ArtifactDirs artifacts("publish_send");
    publish::Publisher publisher(makeClient(node), makeManifest(), artifacts.store);

    const auto owner = makeOwnerSigner();
    const pt::Publication publication = makeRelayedPublication(owner->address(), "relayed_tx");
    const std::string raw = signTransaction(*owner, makePublicationTransaction(publication));

    const auto sent = runAwaitable(io_context, publisher.send(relayClaim(publication), raw));
    ASSERT_TRUE(sent.has_value()) << (sent ? "" : sent.error().message);
    EXPECT_EQ(*sent, publish::transactionHash(raw));
    ASSERT_EQ(node->sent_raw_transactions.size(), 1u);
    EXPECT_EQ(node->sent_raw_transactions.front(), raw);

    // Confirmation then proceeds exactly as for a transaction a wallet sent.
    node->receipt = makeTransformationAddedReceipt(
        *sent, publication.name, owner->address(), publication.content_hash, publication.metadata_hash);
    const auto confirmed = runAwaitable(io_context, publisher.confirm(relayClaim(publication), *sent));
    ASSERT_TRUE(confirmed.has_value()) << (confirmed ? "" : confirmed.error().message);
    EXPECT_EQ(confirmed->tx_hash, *sent);
}

TEST(PublishPublisher, SendRefusesAnythingButExactlyTheOwnersPublication)
{
    using Kind = publish::PublishError::Kind;

    asio::io_context io_context;
    auto node = std::make_shared<FakeNode>();
    ArtifactDirs artifacts("publish_send_refusals");
    publish::Publisher publisher(makeClient(node), makeManifest(), artifacts.store);

    const auto owner = makeOwnerSigner();
    const pt::Publication publication = makeRelayedPublication(owner->address(), "relayed_tx");
    const publish::Transaction transaction = makePublicationTransaction(publication);

    const auto refusal = [&](const publish::PublicationClaim & claim, const publish::Transaction & candidate)
        -> std::optional<Kind>
    {
        const auto result = runAwaitable(io_context, publisher.send(claim, signTransaction(*owner, candidate)));
        if(result) return std::nullopt;
        return result.error().kind;
    };

    // Signed by the owner, but sent by a caller claiming it as their own.
    publish::PublicationClaim other_caller = relayClaim(publication);
    other_caller.owner = makeAddressFromByte(0x43);
    EXPECT_EQ(refusal(other_caller, transaction), Kind::FORBIDDEN);

    publish::Transaction other_chain = transaction;
    other_chain.chain_id = 5;
    EXPECT_EQ(refusal(relayClaim(publication), other_chain), Kind::INVALID);

    publish::Transaction other_contract = transaction;
    other_contract.to = makeAddressFromByte(0xBA);
    EXPECT_EQ(refusal(relayClaim(publication), other_contract), Kind::INVALID);

    publish::Transaction with_value = transaction;
    with_value.value = 1;
    EXPECT_EQ(refusal(relayClaim(publication), with_value), Kind::INVALID);

    publish::Transaction other_call = transaction;
    other_call.data = pt::encodePublicationNonceCall(owner->address());
    EXPECT_EQ(refusal(relayClaim(publication), other_call), Kind::INVALID);

    // A publication other than the one the request names.
    publish::PublicationClaim renamed = relayClaim(publication);
    renamed.name = "other_tx";
    EXPECT_EQ(refusal(renamed, transaction), Kind::INVALID);
    publish::PublicationClaim other_content = relayClaim(publication);
    other_content.content_hash = chain::bytes32ToHex(bytes32OfFill(0x11));
    EXPECT_EQ(refusal(other_content, transaction), Kind::INVALID);
    publish::PublicationClaim other_kind = relayClaim(publication);
    other_kind.kind = pt::EntityKind::CONDITION;
    EXPECT_EQ(refusal(other_kind, transaction), Kind::INVALID);

    // Too little gas to succeed would still cost the owner.
    publish::Transaction low_gas = transaction;
    low_gas.gas_limit = node->gas_estimate - 1;
    EXPECT_EQ(refusal(relayClaim(publication), low_gas), Kind::INVALID);

    node->estimate_error = rpc::RpcError{
        .kind = rpc::RpcError::Kind::EXECUTION,
        .code = 3,
        .message = "execution reverted"};
    EXPECT_EQ(refusal(relayClaim(publication), transaction), Kind::REVERTED);
    node->estimate_error.reset();

    const auto malformed = runAwaitable(io_context, publisher.send(relayClaim(publication), "0x1234"));
    ASSERT_FALSE(malformed.has_value());
    EXPECT_EQ(malformed.error().kind, Kind::INVALID);

    EXPECT_TRUE(node->sent_raw_transactions.empty());
}

TEST(PublishPublisher, SendMapsTheProvidersAnswers)
{
    using Kind = publish::PublishError::Kind;

    asio::io_context io_context;
    auto node = std::make_shared<FakeNode>();
    ArtifactDirs artifacts("publish_send_provider");
    publish::Publisher publisher(makeClient(node), makeManifest(), artifacts.store);

    const auto owner = makeOwnerSigner();
    const pt::Publication publication = makeRelayedPublication(owner->address(), "relayed_tx");
    const std::string raw = signTransaction(*owner, makePublicationTransaction(publication));
    const std::string tx_hash = publish::transactionHash(raw);

    const auto sendAgainst = [&](const int code, const std::string & message)
    {
        node->send_error = rpc::RpcError{.kind = rpc::RpcError::Kind::EXECUTION, .code = code, .message = message};
        return runAwaitable(io_context, publisher.send(relayClaim(publication), raw));
    };

    // A retry of a transaction the provider already holds is the same broadcast.
    const auto known = sendAgainst(-32000, "already known");
    ASSERT_TRUE(known.has_value()) << (known ? "" : known.error().message);
    EXPECT_EQ(*known, tx_hash);

    // Another transaction used the account nonce: the owner prepares again.
    const auto stale = sendAgainst(-32000, "nonce too low: next nonce 8, tx nonce 7");
    ASSERT_FALSE(stale.has_value());
    EXPECT_EQ(stale.error().kind, Kind::CONFLICT);

    // Unless this very transaction used it and was already mined.
    node->receipt = makeTransformationAddedReceipt(
        tx_hash, publication.name, owner->address(), publication.content_hash, publication.metadata_hash);
    const auto mined = sendAgainst(-32000, "nonce too low");
    ASSERT_TRUE(mined.has_value()) << (mined ? "" : mined.error().message);
    EXPECT_EQ(*mined, tx_hash);
    node->receipt = json{};

    EXPECT_EQ(sendAgainst(-32000, "replacement transaction underpriced").error().kind, Kind::CONFLICT);
    EXPECT_EQ(sendAgainst(-32000, "insufficient funds for gas * price + value").error().kind, Kind::INVALID);
    EXPECT_EQ(sendAgainst(-32000, "intrinsic gas too low").error().kind, Kind::INVALID);
    EXPECT_EQ(sendAgainst(-32603, "internal error").error().kind, Kind::UPSTREAM);
    EXPECT_TRUE(node->sent_raw_transactions.empty());
}

// ---------------------------------------------------------------------------
// Bootstrap
// ---------------------------------------------------------------------------

TEST(PublishBootstrap, DeploysFourContractsWithSequentialNoncesAndVerifiesThem)
{
    asio::io_context io_context;
    const auto signer = makeSigner();
    const auto paths = makeTempEventsPaths("publish_bootstrap");
    const auto artifacts = writeBootstrapArtifacts(paths.root / "bin");

    auto node = std::make_shared<FakeNode>();
    node->mine_deployments = true;
    // Deployment order: registry impl (0xa0), registry proxy (0xa1), runner impl (0xa2),
    // runner proxy (0xa3).
    node->implementation_by_proxy[hexAddress(0xA1)] = wordOfAddress(hexAddress(0xA0));
    node->implementation_by_proxy[hexAddress(0xA3)] = wordOfAddress(hexAddress(0xA2));
    node->runner_registry_word = wordOfAddress(hexAddress(0xA1));
    node->owner_word = wordOfAddress(chain::addressToHex(signer->address()));

    const auto manifest = runAwaitable(io_context, publish::bootstrapDeployment(
        *makeClient(node), *signer, artifacts, CHAIN_ID, "0.8.30", "revision"));
    ASSERT_TRUE(manifest.has_value()) << (manifest ? "" : manifest.error());

    EXPECT_EQ(manifest->registry_impl, hexAddress(0xA0));
    EXPECT_EQ(manifest->registry, hexAddress(0xA1));
    EXPECT_EQ(manifest->runner_impl, hexAddress(0xA2));
    EXPECT_EQ(manifest->runner, hexAddress(0xA3));
    EXPECT_EQ(manifest->deployment_block, 52);   // the registry proxy's block
    EXPECT_EQ(manifest->owner, chain::addressToHex(signer->address()));

    // One transaction per contract, each with its own nonce.
    ASSERT_EQ(node->sent_raw_transactions.size(), 4u);
    std::set<std::string> distinct(node->sent_raw_transactions.begin(), node->sent_raw_transactions.end());
    EXPECT_EQ(distinct.size(), 4u);
}

TEST(PublishBootstrap, RefusesToSpendAboveTheConfiguredCeilings)
{
    asio::io_context io_context;
    const auto signer = makeSigner();
    const auto paths = makeTempEventsPaths("publish_bootstrap_ceiling");
    const auto artifacts = writeBootstrapArtifacts(paths.root / "bin");

    auto node = std::make_shared<FakeNode>();
    node->mine_deployments = true;
    node->gas_estimate = 5'000'000;

    const auto over_gas = runAwaitable(io_context, publish::bootstrapDeployment(
        *makeClient(node), *signer, artifacts, CHAIN_ID, "0.8.30", "revision",
        publish::BootstrapConfig{.max_gas = 1'000'000}));
    EXPECT_FALSE(over_gas.has_value());

    node->gas_estimate = 150'000;
    node->base_fee = 500'000'000'000;
    const auto over_fee = runAwaitable(io_context, publish::bootstrapDeployment(
        *makeClient(node), *signer, artifacts, CHAIN_ID, "0.8.30", "revision"));
    EXPECT_FALSE(over_fee.has_value());

    EXPECT_TRUE(node->sent_raw_transactions.empty());
}
