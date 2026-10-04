#include "unit-tests.hpp"

#include "events_test_harness.hpp"

#include <algorithm>
#include <format>
#include <fstream>
#include <limits>
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
        const std::filesystem::path key_path = buildPath() / std::format("publish_tests_operator_{}.key", currentTestName());
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
        request.publication.kind = pt::EntityKind::TRANSFORMATION;
        request.publication.name = "published_tx";
        request.publication.owner = owner;
        request.publication.content_hash = pt::computeContentHash(
            makeArtifactBytes(pt::EntityKind::TRANSFORMATION, "published_tx"));
        request.publication.metadata_hash = bytes32OfFill(0xBB);
        request.publication.creation_code_hash = bytes32OfFill(0xCC);
        request.publication.runtime_code_hash = bytes32OfFill(0xDD);
        request.creation_code = std::vector<std::uint8_t>{0x60, 0x00, 0x60, 0x00, 0xF3};
        return request;
    }

    publish::PublicationRequest makeConnectorRequest(const chain::Address & owner)
    {
        publish::PublicationRequest request;
        request.publication.kind = pt::EntityKind::CONNECTOR;
        request.publication.name = "published_connector";
        request.publication.owner = owner;
        request.publication.content_hash = pt::computeContentHash(
            makeArtifactBytes(pt::EntityKind::CONNECTOR, "published_connector"));
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

    // Compiler output for both implementations and the proxy they share.
    struct DeployArtifacts
    {
        std::filesystem::path registry_impl_bin;
        std::filesystem::path runner_impl_bin;
        std::filesystem::path proxy_bin;
    };

    DeployArtifacts writeDeployArtifacts(const std::filesystem::path & dir)
    {
        std::filesystem::create_directories(dir);
        const DeployArtifacts artifacts{
            .registry_impl_bin = dir / "RegistryBase.bin",
            .runner_impl_bin = dir / "Runner.bin",
            .proxy_bin = dir / "PTContractProxy.bin"};
        for(const auto & path : {artifacts.registry_impl_bin, artifacts.runner_impl_bin, artifacts.proxy_bin})
        {
            std::ofstream(path, std::ios::trunc) << "6000600055\n";
        }
        return artifacts;
    }

    // The field payloads of a raw type-2 transaction. decodeSignedTransaction refuses
    // contract creations, which are exactly what a deployment sends.
    std::vector<std::vector<std::uint8_t>> rawTransactionFields(const std::string & raw_hex)
    {
        const auto raw = evmc::from_hex(raw_hex).value();
        std::size_t pos = 1;  // past the type byte
        const auto readLength = [&]()
        {
            const std::uint8_t prefix = raw[pos];
            if(prefix < 0x80) return std::size_t{1};  // a single byte is its own payload
            ++pos;
            const std::uint8_t base = prefix >= 0xC0 ? 0xC0 : 0x80;
            if(prefix - base <= 55) return static_cast<std::size_t>(prefix - base);
            std::size_t length = 0;
            for(int width = prefix - base - 55; width > 0; --width) length = (length << 8) | raw[pos++];
            return length;
        };
        const std::size_t end = pos + readLength();
        std::vector<std::vector<std::uint8_t>> fields;
        while(pos < end)
        {
            const std::size_t length = readLength();
            fields.emplace_back(raw.begin() + pos, raw.begin() + pos + length);
            pos += length;
        }
        return fields;
    }

    publish::DeploymentManifest withoutRunner(publish::DeploymentManifest manifest)
    {
        manifest.runner.clear();
        manifest.runner_impl.clear();
        manifest.runner_impl_code_hash.clear();
        manifest.runner_tx.clear();
        return manifest;
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

TEST(PublishSigner, DeployerEnvironmentIsUnsetByDefault)
{
    const config::PublishConfig cfg;
    ASSERT_TRUE(cfg.deployer_key_env.empty());
    EXPECT_FALSE(publish::Signer::fromEnvironment(cfg.deployer_key_env).has_value());
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

namespace
{
    // `call` with its publication's deadline set to type(uint256).max.
    std::vector<std::uint8_t> withMaxDeadline(std::vector<std::uint8_t> call)
    {
        const std::size_t tuple = 4 + chain::readWordAsSizeT(call.data() + 4, call.size() - 4, 32).value();
        std::fill_n(call.begin() + static_cast<std::ptrdiff_t>(tuple + 9 * 32), 32, std::uint8_t{0xFF});
        return call;
    }
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

    // A uint256 deadline beyond 64 bits, such as type(uint256).max, never expires.
    pt::Publication unbounded = publication;
    unbounded.kind = pt::EntityKind::TRANSFORMATION;
    const auto unbounded_decoded = pt::decodePublishCall(withMaxDeadline(encode(unbounded)));
    ASSERT_TRUE(unbounded_decoded.has_value());
    EXPECT_EQ(unbounded_decoded->deadline, std::numeric_limits<std::uint64_t>::max());

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

namespace
{
    // One transformation publication and the publish call that carries it.
    struct PublishedFixture
    {
        std::vector<std::uint8_t> creation_code{0x60, 0x80, 0x60, 0x40, 0x52, 0x34};
        chain::Address registry = evmc::from_hex<chain::Address>(REGISTRY_ADDRESS).value();
        pt::Publication publication;
        chain::Address entity{};
        std::vector<std::uint8_t> publish_call;

        PublishedFixture()
        {
            publication.kind = pt::EntityKind::TRANSFORMATION;
            publication.name = "found_tf";
            publication.owner = makeAddressFromByte(0x42);
            publication.content_hash = bytes32OfFill(0xC0);
            publication.metadata_hash = bytes32OfFill(0xC1);
            publication.creation_code_hash = chain::keccakBytes(creation_code);
            publication.runtime_code_hash = bytes32OfFill(0xC2);
            entity = pt::predictEntityAddress(registry, publication, creation_code);
            publish_call = pt::encodePublishTransformationCall(creation_code, publication, {});
        }

        // `call` as the bytes argument of a relay call, behind three unrelated bytes.
        static std::vector<std::uint8_t> relayed(const std::vector<std::uint8_t> & call)
        {
            std::vector<std::uint8_t> out{0xDE, 0xAD, 0xBE};
            const std::vector<std::uint8_t> selector = chain::constructSelector("execute(address,bytes)");
            out.insert(out.end(), selector.begin(), selector.end());
            const std::vector<std::uint8_t> arguments = pt::abi::encodeTuple({
                pt::abi::addressComponent(makeAddressFromByte(0x99)),
                pt::abi::bytesComponent(call)});
            out.insert(out.end(), arguments.begin(), arguments.end());
            return out;
        }
    };
}

// The creation code behind a registry event is found in the transaction's own publish call
// or in one a relay embeds, by the CREATE2 address the registry deploys it to.
TEST(PublishPublication, FindsTheCreationCodeOfAPublishCallDirectOrEmbedded)
{
    const PublishedFixture fixture;
    const auto codeOf = [&](const std::vector<std::uint8_t> & calldata, const chain::Address & registry)
        -> std::optional<std::vector<std::uint8_t>>
    {
        const auto codes = pt::findPublishedCreationCodes(calldata, registry);
        const auto found = codes.find(fixture.entity);
        if(found == codes.end()) return std::nullopt;
        return found->second;
    };
    const auto expected = std::optional<std::vector<std::uint8_t>>(fixture.creation_code);

    EXPECT_EQ(codeOf(fixture.publish_call, fixture.registry), expected);
    EXPECT_EQ(codeOf(PublishedFixture::relayed(fixture.publish_call), fixture.registry), expected);
    EXPECT_EQ(codeOf(withMaxDeadline(fixture.publish_call), fixture.registry), expected);

    // Another registry deploys the same publication elsewhere.
    EXPECT_FALSE(codeOf(fixture.publish_call, makeAddressFromByte(0xBA)));

    // A decoy call ahead of the real one, with the same name, owner, nonce and hashes but
    // other creation code that hashes to the creation code hash it claims. It deploys
    // elsewhere, so it never stands in for the entity's code.
    const std::vector<std::uint8_t> decoy_code{0x60, 0x80, 0x60, 0x40, 0x52, 0x35};
    pt::Publication decoy = fixture.publication;
    decoy.creation_code_hash = chain::keccakBytes(decoy_code);
    const std::vector<std::uint8_t> decoy_call = pt::encodePublishTransformationCall(decoy_code, decoy, {});
    std::vector<std::uint8_t> batched = decoy_call;
    batched.insert(batched.end(), fixture.publish_call.begin(), fixture.publish_call.end());
    EXPECT_EQ(codeOf(batched, fixture.registry), expected);
    EXPECT_FALSE(codeOf(decoy_call, fixture.registry));

    // Every publish call in the calldata is indexed, each by its own address.
    const auto both = pt::findPublishedCreationCodes(batched, fixture.registry);
    EXPECT_EQ(both.size(), 2u);
    EXPECT_TRUE(both.contains(pt::predictEntityAddress(fixture.registry, decoy, decoy_code)));

    // Calldata without the call, or cut short.
    EXPECT_FALSE(codeOf(std::vector<std::uint8_t>{}, fixture.registry));
    EXPECT_FALSE(codeOf(std::vector<std::uint8_t>{0x12, 0x34, 0x56, 0x78}, fixture.registry));
    std::vector<std::uint8_t> truncated = fixture.publish_call;
    truncated.resize(truncated.size() / 2);
    EXPECT_FALSE(codeOf(truncated, fixture.registry));
}

TEST(PtReads, ReadsTheCreationCodeOfAPublicationTransaction)
{
    asio::io_context io_context;
    const PublishedFixture fixture;
    auto node = std::make_shared<FakeNode>();
    const std::string block_hash = hexBytes(0xAA, 32);
    node->addTransaction(hexBytes(0x01, 32), block_hash, PublishedFixture::relayed(fixture.publish_call));
    node->addTransaction(hexBytes(0x02, 32), block_hash, {0x12, 0x34});
    const auto client = makeClient(node);

    const auto read = [&](const std::string & tx_hash, const std::string & block)
    {
        return runAwaitable(io_context, rpc::readPublishedCreationCodes(*client, tx_hash, block, fixture.registry));
    };

    const auto relayed = read(hexBytes(0x01, 32), block_hash);
    ASSERT_TRUE(relayed.has_value()) << relayed.error().message;
    ASSERT_TRUE(relayed->contains(fixture.entity));
    EXPECT_EQ(relayed->at(fixture.entity), fixture.creation_code);

    // No publish call in the calldata: nothing to read, and no error.
    const auto without_call = read(hexBytes(0x02, 32), block_hash);
    ASSERT_TRUE(without_call.has_value());
    EXPECT_TRUE(without_call->empty());

    // The event names the transaction and its block: anything else is an error.
    EXPECT_FALSE(read(hexBytes(0x03, 32), block_hash).has_value());
    EXPECT_FALSE(read(hexBytes(0x01, 32), hexBytes(0xBB, 32)).has_value());
}

// A back-filled entity's publication is found from the block its code first appears in,
// with a binary search rather than a scan of the registry's history.
TEST(PtReads, LocatesAnEntityPublicationFromItsCreationBlock)
{
    asio::io_context io_context;
    auto node = std::make_shared<FakeNode>();
    const chain::Address entity = makeAddressFromByte(0x43);
    node->code_created_at[chain::addressToHex(entity)] = 37;

    const std::string topic = pt::eventTopic(pt::TRANSFORMATION_ADDED_SIGNATURE);
    const auto log = [&](const std::string & name, const chain::Address & address, const std::uint8_t tx_byte)
    {
        return json{
            {"address", REGISTRY_ADDRESS},
            {"topics", json::array({topic})},
            {"data", encodeSimpleAddedEventDataV2(makeAddressFromByte(0x41), name, address, makeAddressFromByte(0x42), 0)},
            {"transactionHash", hexBytes(tx_byte, 32)},
            {"blockHash", hexBytes(0x37, 32)}};
    };
    node->logs_by_block_number[37] = json::array({
        log("unrelated_tf", makeAddressFromByte(0x44), 0x01),
        log("located_tf", entity, 0x02)});
    const auto client = makeClient(node);

    const auto located = runAwaitable(io_context, rpc::findEntityPublication(
        *client, REGISTRY_ADDRESS, pt::EntityKind::TRANSFORMATION, "located_tf", entity, 100));
    ASSERT_TRUE(located.has_value()) << located.error().message;
    ASSERT_TRUE(located->has_value());
    EXPECT_EQ((**located).tx_hash, hexBytes(0x02, 32));
    EXPECT_EQ((**located).block_hash, hexBytes(0x37, 32));
    // One read at the latest block, then a binary search over 101 blocks.
    EXPECT_LE(node->code_requests.size(), 9u);

    // An entity without code at the latest block, or with no matching event, is not found.
    node->code_created_at[chain::addressToHex(entity)] = 200;
    const auto absent = runAwaitable(io_context, rpc::findEntityPublication(
        *client, REGISTRY_ADDRESS, pt::EntityKind::TRANSFORMATION, "located_tf", entity, 100));
    ASSERT_TRUE(absent.has_value());
    EXPECT_FALSE(absent->has_value());
    node->code_created_at[chain::addressToHex(entity)] = 37;
    const auto unnamed = runAwaitable(io_context, rpc::findEntityPublication(
        *client, REGISTRY_ADDRESS, pt::EntityKind::TRANSFORMATION, "other_tf", entity, 100));
    ASSERT_TRUE(unnamed.has_value());
    EXPECT_FALSE(unnamed->has_value());
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

    // A registry without a runner is a complete manifest.
    ASSERT_TRUE(publish::writeManifest(path, withoutRunner(manifest)).has_value());
    const auto registry_only = publish::readManifest(path);
    ASSERT_TRUE(registry_only.has_value()) << (registry_only ? "" : registry_only.error());
    EXPECT_EQ(registry_only->registry, manifest.registry);
    EXPECT_TRUE(registry_only->runner.empty());
    EXPECT_TRUE(registry_only->runner_impl.empty());
    EXPECT_TRUE(registry_only->runner_impl_code_hash.empty());
    EXPECT_TRUE(registry_only->runner_tx.empty());

    auto omitted = json::parse(publish::formatManifest(withoutRunner(manifest)));
    for(const char * field : {"runner", "runner_impl", "runner_impl_code_hash", "runner_tx"}) omitted.erase(field);
    {
        std::ofstream output(path, std::ios::trunc);
        output << omitted.dump();
        ASSERT_TRUE(output.good());
    }
    const auto without_fields = publish::readManifest(path);
    ASSERT_TRUE(without_fields.has_value());
    EXPECT_TRUE(without_fields->runner.empty());
    EXPECT_TRUE(without_fields->runner_impl.empty());
    EXPECT_TRUE(without_fields->runner_impl_code_hash.empty());
    EXPECT_TRUE(without_fields->runner_tx.empty());
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

    // The runner fields are all set or all empty.
    publish::DeploymentManifest partial_runner = makeManifest();
    partial_runner.runner_impl.clear();
    ASSERT_TRUE(publish::writeManifest(path, partial_runner).has_value());
    EXPECT_FALSE(publish::readManifest(path).has_value());

    publish::DeploymentManifest no_registry = makeManifest();
    no_registry.registry.clear();
    ASSERT_TRUE(publish::writeManifest(path, no_registry).has_value());
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

    // So must a runner proxy that drifted.
    node->owner_word = wordOfAddress(hexAddress(0x77));
    node->implementation_by_proxy[RUNNER_ADDRESS] = wordOfAddress(hexAddress(0x01));
    EXPECT_FALSE(runAwaitable(io_context, publish::verifyDeployment(*makeClient(node), manifest)).has_value());

    // A manifest without a runner verifies its registry alone.
    const auto registry_only = runAwaitable(
        io_context, publish::verifyDeployment(*makeClient(node), withoutRunner(manifest)));
    EXPECT_TRUE(registry_only.has_value()) << (registry_only ? "" : registry_only.error());
}

TEST(PublishManifest, VerifiesAnAddressOnlyRunner)
{
    asio::io_context io_context;
    auto node = std::make_shared<FakeNode>();

    EXPECT_TRUE(runAwaitable(io_context, rpc::verifyPtRunner(*makeClient(node), RUNNER_ADDRESS)).has_value());

    node->call_override = [](const json & request)
    {
        return rpcResult(request, wordOfFill(0x00));
    };
    EXPECT_FALSE(runAwaitable(io_context, rpc::verifyPtRunner(*makeClient(node), RUNNER_ADDRESS)).has_value());
}

// ---------------------------------------------------------------------------
// Publisher: preparation
// ---------------------------------------------------------------------------

TEST(PublishPublisher, PreparesAnUnsignedTransactionTheOwnerSends)
{
    asio::io_context io_context;
    auto node = std::make_shared<FakeNode>();
    publish::Publisher publisher(makeClient(node), makeManifest());

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

    // The server never sends anything itself.
    EXPECT_TRUE(node->sent_raw_transactions.empty());
}

TEST(PublishPublisher, ReportsADeterministicRevertBeforeTheOwnerPays)
{
    asio::io_context io_context;
    auto node = std::make_shared<FakeNode>();
    node->estimate_error = rpc::RpcError{
        .kind = rpc::RpcError::Kind::EXECUTION,
        .code = 3,
        .message = "execution reverted"};
    publish::Publisher publisher(makeClient(node), makeManifest());

    const auto result = runAwaitable(
        io_context, publisher.prepare(makeTransformationRequest(makeAddressFromByte(0x42))));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, publish::PublishError::Kind::REVERTED);
}

TEST(PublishPublisher, ConnectorDependenciesMustBeTheArtifactsOnTheTargetRegistry)
{
    asio::io_context io_context;
    auto node = std::make_shared<FakeNode>();
    node->content_hash_by_name["dep_tx"] = wordOfFill(0x00);        // not registered
    node->content_hash_by_name["dep_cond"] = wordOfFill(0x11);      // registered, other content
    node->content_hash_by_name["dep_ok"] = wordOfFill(0x22);
    publish::Publisher publisher(makeClient(node), makeManifest());

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
    publish::Publisher publisher(makeClient(node), makeManifest());

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
    publish::Publisher publisher(makeClient(node), makeManifest());

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
    publish::Publisher publisher(makeClient(node), makeManifest());

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
    publish::Publisher publisher(makeClient(node), makeManifest());

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
    publish::Publisher publisher(makeClient(node), makeManifest());
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
    publish::Publisher publisher(makeClient(node), makeManifest());

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
    publish::Publisher publisher(makeClient(node), makeManifest());

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
    publish::Publisher publisher(makeClient(node), makeManifest());

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
// Contract deployment and upgrade
// ---------------------------------------------------------------------------

namespace
{
    // A node serving the makeManifest() deployment, owned by `owner`, that a manifest
    // from operatorManifest() verifies against.
    std::shared_ptr<FakeNode> makeDeployedNode(const publish::Signer & owner)
    {
        auto node = std::make_shared<FakeNode>();
        node->mine_deployments = true;
        node->implementation_by_proxy[REGISTRY_ADDRESS] = wordOfAddress(REGISTRY_IMPL_ADDRESS);
        node->implementation_by_proxy[RUNNER_ADDRESS] = wordOfAddress(RUNNER_IMPL_ADDRESS);
        node->owner_word = wordOfAddress(chain::addressToHex(owner.address()));
        return node;
    }

    publish::DeploymentManifest operatorManifest(const FakeNode & node, const publish::Signer & owner)
    {
        publish::DeploymentManifest manifest = makeManifest();
        // The fake node reports the same code for every address, so pin the manifest hashes to it.
        const auto code_bytes = evmc::from_hex(node.code).value();
        const std::string code_hash =
            chain::bytes32ToHex(chain::keccakBytes(code_bytes.data(), code_bytes.size()));
        manifest.registry_impl_code_hash = code_hash;
        manifest.runner_impl_code_hash = code_hash;
        manifest.registry_tx = hexBytes(0x60, 32);
        manifest.runner_tx = hexBytes(0x61, 32);
        manifest.owner = chain::addressToHex(owner.address());
        return manifest;
    }

    std::string proxyOf(const publish::PtContract contract)
    {
        return contract == publish::PtContract::REGISTRY ? REGISTRY_ADDRESS : RUNNER_ADDRESS;
    }

    std::filesystem::path implementationOf(const DeployArtifacts & artifacts, const publish::PtContract contract)
    {
        return contract == publish::PtContract::REGISTRY ? artifacts.registry_impl_bin : artifacts.runner_impl_bin;
    }
}

TEST(PublishDeploy, DeploysARegistryThenARunnerWithSequentialNoncesAndVerifiesThem)
{
    asio::io_context io_context;
    const auto signer = makeSigner();
    const auto paths = makeTempEventsPaths("publish_deploy");
    const auto artifacts = writeDeployArtifacts(paths.root / "bin");

    auto node = std::make_shared<FakeNode>();
    node->mine_deployments = true;
    // A provider's pending transaction count advances with every accepted transaction.
    FakeNode & chain_state = *node;
    node->on_send = [&chain_state](const std::string &) { ++chain_state.tx_count; };
    // Deployment order: registry impl (0xa0), registry proxy (0xa1), runner impl (0xa2),
    // runner proxy (0xa3).
    node->implementation_by_proxy[hexAddress(0xA1)] = wordOfAddress(hexAddress(0xA0));
    node->implementation_by_proxy[hexAddress(0xA3)] = wordOfAddress(hexAddress(0xA2));
    node->owner_word = wordOfAddress(chain::addressToHex(signer->address()));

    const auto with_registry = runAwaitable(io_context, publish::deployContract(
        *makeClient(node), *signer, publish::PtContract::REGISTRY,
        artifacts.registry_impl_bin, artifacts.proxy_bin,
        publish::DeploymentManifest{.chain_id = CHAIN_ID}));
    ASSERT_TRUE(with_registry.has_value()) << (with_registry ? "" : with_registry.error());

    EXPECT_EQ(with_registry->registry_impl, hexAddress(0xA0));
    EXPECT_EQ(with_registry->registry, hexAddress(0xA1));
    EXPECT_EQ(with_registry->deployment_block, 52);   // the registry proxy's block
    EXPECT_EQ(with_registry->protocol_id, rpc::ptProtocolId());
    EXPECT_EQ(with_registry->protocol_version, rpc::PT_PROTOCOL_VERSION);
    EXPECT_EQ(with_registry->owner, chain::addressToHex(signer->address()));
    // A registry alone is a complete deployment; a runner is added separately.
    EXPECT_TRUE(with_registry->runner.empty());

    const auto deployed = runAwaitable(io_context, publish::deployContract(
        *makeClient(node), *signer, publish::PtContract::RUNNER,
        artifacts.runner_impl_bin, artifacts.proxy_bin, *with_registry));
    ASSERT_TRUE(deployed.has_value()) << (deployed ? "" : deployed.error());

    EXPECT_EQ(deployed->runner_impl, hexAddress(0xA2));
    EXPECT_EQ(deployed->runner, hexAddress(0xA3));
    EXPECT_EQ(deployed->registry, hexAddress(0xA1));
    EXPECT_EQ(deployed->deployment_block, 52);

    // One contract creation per contract, with consecutive nonces.
    ASSERT_EQ(node->sent_raw_transactions.size(), 4u);
    for(std::size_t i = 0; i < node->sent_raw_transactions.size(); ++i)
    {
        const auto fields = rawTransactionFields(node->sent_raw_transactions[i]);
        ASSERT_EQ(fields.size(), 12u);
        std::uint64_t nonce = 0;
        for(const std::uint8_t byte : fields[1]) nonce = (nonce << 8) | byte;
        EXPECT_EQ(nonce, 7u + i);
        EXPECT_TRUE(fields[5].empty());
    }

    // The runner proxy is created with its implementation as the only constructor
    // argument: it is bound to no registry.
    const auto creation = rawTransactionFields(node->sent_raw_transactions[3])[7];
    const auto implementation_word = encodeAddressWord(makeAddressFromByte(0xA2));
    ASSERT_GE(creation.size(), implementation_word.size());
    EXPECT_TRUE(std::equal(implementation_word.begin(), implementation_word.end(), creation.end() - 32));
}

TEST(PublishDeploy, RefusesToSpendAboveTheConfiguredCeilings)
{
    asio::io_context io_context;
    const auto signer = makeSigner();
    const auto paths = makeTempEventsPaths("publish_deploy_ceiling");
    const auto artifacts = writeDeployArtifacts(paths.root / "bin");

    auto node = std::make_shared<FakeNode>();
    node->mine_deployments = true;
    node->gas_estimate = 5'000'000;

    const auto over_gas = runAwaitable(io_context, publish::deployContract(
        *makeClient(node), *signer, publish::PtContract::REGISTRY,
        artifacts.registry_impl_bin, artifacts.proxy_bin,
        publish::DeploymentManifest{.chain_id = CHAIN_ID},
        publish::DeployConfig{.max_gas = 1'000'000}));
    EXPECT_FALSE(over_gas.has_value());

    node->gas_estimate = 150'000;
    node->base_fee = 500'000'000'000;
    const auto over_fee = runAwaitable(io_context, publish::deployContract(
        *makeClient(node), *signer, publish::PtContract::REGISTRY,
        artifacts.registry_impl_bin, artifacts.proxy_bin,
        publish::DeploymentManifest{.chain_id = CHAIN_ID}));
    EXPECT_FALSE(over_fee.has_value());

    EXPECT_TRUE(node->sent_raw_transactions.empty());
}

TEST(PublishDeploy, ReplacesEitherContractAndKeepsTheOther)
{
    for(const auto contract : {publish::PtContract::REGISTRY, publish::PtContract::RUNNER})
    {
        SCOPED_TRACE(publish::contractLabel(contract));
        asio::io_context io_context;
        const auto signer = makeSigner();
        const auto paths = makeTempEventsPaths(std::format("publish_replace_{}", publish::contractLabel(contract)));
        const auto artifacts = writeDeployArtifacts(paths.root / "bin");

        auto node = makeDeployedNode(*signer);
        const publish::DeploymentManifest manifest = operatorManifest(*node, *signer);
        // Deployment order: implementation (0xa0), proxy (0xa1).
        node->implementation_by_proxy[hexAddress(0xA1)] = wordOfAddress(hexAddress(0xA0));
        // The contract being replaced may have drifted; only the one that stays must verify.
        node->implementation_by_proxy[proxyOf(contract)] = wordOfAddress(hexAddress(0x01));

        const auto updated = runAwaitable(io_context, publish::deployContract(
            *makeClient(node), *signer, contract,
            implementationOf(artifacts, contract), artifacts.proxy_bin, manifest));
        ASSERT_TRUE(updated.has_value()) << (updated ? "" : updated.error());
        ASSERT_EQ(node->sent_raw_transactions.size(), 2u);
        const std::string proxy_tx = chain::normalizeHex(publish::transactionHash(node->sent_raw_transactions[1]));
        EXPECT_EQ(updated->owner, manifest.owner);

        if(contract == publish::PtContract::REGISTRY)
        {
            EXPECT_EQ(updated->registry, hexAddress(0xA1));
            EXPECT_EQ(updated->registry_impl, hexAddress(0xA0));
            EXPECT_EQ(updated->registry_tx, proxy_tx);
            EXPECT_EQ(updated->deployment_block, 52);
            EXPECT_EQ(updated->runner, manifest.runner);
            EXPECT_EQ(updated->runner_impl, manifest.runner_impl);
            EXPECT_EQ(updated->runner_impl_code_hash, manifest.runner_impl_code_hash);
            EXPECT_EQ(updated->runner_tx, manifest.runner_tx);
        }
        else
        {
            EXPECT_EQ(updated->runner, hexAddress(0xA1));
            EXPECT_EQ(updated->runner_impl, hexAddress(0xA0));
            EXPECT_EQ(updated->runner_tx, proxy_tx);
            EXPECT_EQ(updated->registry, manifest.registry);
            EXPECT_EQ(updated->registry_impl, manifest.registry_impl);
            EXPECT_EQ(updated->registry_impl_code_hash, manifest.registry_impl_code_hash);
            EXPECT_EQ(updated->registry_tx, manifest.registry_tx);
            EXPECT_EQ(updated->deployment_block, manifest.deployment_block);
        }
    }
}

TEST(PublishDeploy, AddsARunnerOnlyToADeploymentWithARegistry)
{
    asio::io_context io_context;
    const auto signer = makeSigner();
    const auto paths = makeTempEventsPaths("publish_deploy_runner_needs_registry");
    const auto artifacts = writeDeployArtifacts(paths.root / "bin");

    auto node = makeDeployedNode(*signer);
    const auto refused = runAwaitable(io_context, publish::deployContract(
        *makeClient(node), *signer, publish::PtContract::RUNNER,
        artifacts.runner_impl_bin, artifacts.proxy_bin,
        publish::DeploymentManifest{.chain_id = CHAIN_ID}));
    EXPECT_FALSE(refused.has_value());
    EXPECT_TRUE(node->sent_raw_transactions.empty());

    // A registry-only manifest gains a runner.
    node->implementation_by_proxy[hexAddress(0xA1)] = wordOfAddress(hexAddress(0xA0));
    const auto added = runAwaitable(io_context, publish::deployContract(
        *makeClient(node), *signer, publish::PtContract::RUNNER,
        artifacts.runner_impl_bin, artifacts.proxy_bin, withoutRunner(operatorManifest(*node, *signer))));
    ASSERT_TRUE(added.has_value()) << (added ? "" : added.error());
    EXPECT_EQ(added->runner, hexAddress(0xA1));
}

TEST(PublishDeploy, RefusesAnotherAccountsDeploymentWithoutSpendingGas)
{
    asio::io_context io_context;
    const auto signer = makeSigner();
    const auto paths = makeTempEventsPaths("publish_deploy_foreign");
    const auto artifacts = writeDeployArtifacts(paths.root / "bin");

    auto node = makeDeployedNode(*signer);
    publish::DeploymentManifest foreign = operatorManifest(*node, *signer);
    foreign.owner = hexAddress(0x77);

    for(const auto contract : {publish::PtContract::REGISTRY, publish::PtContract::RUNNER})
    {
        SCOPED_TRACE(publish::contractLabel(contract));
        EXPECT_FALSE(runAwaitable(io_context, publish::deployContract(
            *makeClient(node), *signer, contract,
            implementationOf(artifacts, contract), artifacts.proxy_bin, foreign)).has_value());
    }
    EXPECT_TRUE(node->sent_raw_transactions.empty());
}

TEST(PublishDeploy, RefusesToDeployARunnerNextToAnUnverifiedRegistryWithoutSpendingGas)
{
    asio::io_context io_context;
    const auto signer = makeSigner();
    const auto paths = makeTempEventsPaths("publish_runner_registry_preflight");
    const auto artifacts = writeDeployArtifacts(paths.root / "bin");

    for(const std::string drift : {"chain", "protocol", "implementation", "code", "owner", "missing"})
    {
        SCOPED_TRACE(drift);
        auto node = makeDeployedNode(*signer);
        const auto manifest = operatorManifest(*node, *signer);
        if(drift == "chain") ++node->chain_id;
        if(drift == "implementation") node->implementation_by_proxy[REGISTRY_ADDRESS] = wordOfAddress(hexAddress(0x01));
        if(drift == "code") node->code = "0x6001";
        if(drift == "owner") node->owner_word = wordOfAddress(hexAddress(0x02));
        if(drift == "missing") node->code = "0x";
        if(drift == "protocol")
        {
            node->call_override = [](const json & request)
            {
                return rpcResult(request, wordOfFill(0x00));
            };
        }

        const auto deployed = runAwaitable(io_context, publish::deployContract(
            *makeClient(node), *signer, publish::PtContract::RUNNER,
            artifacts.runner_impl_bin, artifacts.proxy_bin, manifest));
        EXPECT_FALSE(deployed.has_value());
        EXPECT_TRUE(node->sent_raw_transactions.empty());
    }
}

TEST(PublishDeploy, RefusesToDeployARegistryNextToAnUnverifiedRunnerWithoutSpendingGas)
{
    asio::io_context io_context;
    const auto signer = makeSigner();
    const auto paths = makeTempEventsPaths("publish_registry_runner_preflight");
    const auto artifacts = writeDeployArtifacts(paths.root / "bin");

    for(const std::string drift : {"protocol", "implementation", "owner"})
    {
        SCOPED_TRACE(drift);
        auto node = makeDeployedNode(*signer);
        const auto manifest = operatorManifest(*node, *signer);
        if(drift == "implementation") node->implementation_by_proxy[RUNNER_ADDRESS] = wordOfAddress(hexAddress(0x01));
        if(drift == "owner") node->owner_word = wordOfAddress(hexAddress(0x02));
        if(drift == "protocol")
        {
            node->call_override = [](const json & request)
            {
                return rpcResult(request, wordOfFill(0x00));
            };
        }

        const auto deployed = runAwaitable(io_context, publish::deployContract(
            *makeClient(node), *signer, publish::PtContract::REGISTRY,
            artifacts.registry_impl_bin, artifacts.proxy_bin, manifest));
        EXPECT_FALSE(deployed.has_value());
        EXPECT_TRUE(node->sent_raw_transactions.empty());
    }
}

TEST(PublishUpgrade, UpgradesEitherProxyInPlace)
{
    for(const auto contract : {publish::PtContract::REGISTRY, publish::PtContract::RUNNER})
    {
        SCOPED_TRACE(publish::contractLabel(contract));
        asio::io_context io_context;
        const auto signer = makeSigner();
        const auto paths = makeTempEventsPaths(std::format("publish_upgrade_{}", publish::contractLabel(contract)));
        const auto artifacts = writeDeployArtifacts(paths.root / "bin");

        auto node = makeDeployedNode(*signer);
        const publish::DeploymentManifest manifest = operatorManifest(*node, *signer);
        const std::string proxy = proxyOf(contract);
        // The implementation deploys at 0xa0; the upgrade call that follows repoints the proxy.
        FakeNode & chain_state = *node;
        node->on_send = [&chain_state, proxy](const std::string &)
        {
            if(chain_state.sent_raw_transactions.size() == 2)
            {
                chain_state.implementation_by_proxy[proxy] = wordOfAddress(hexAddress(0xA0));
            }
        };

        const auto updated = runAwaitable(io_context, publish::upgradeContract(
            *makeClient(node), *signer, contract, implementationOf(artifacts, contract), manifest));
        ASSERT_TRUE(updated.has_value()) << (updated ? "" : updated.error());

        EXPECT_EQ(updated->registry, manifest.registry);
        EXPECT_EQ(updated->runner, manifest.runner);
        EXPECT_EQ(updated->registry_tx, manifest.registry_tx);
        EXPECT_EQ(updated->runner_tx, manifest.runner_tx);
        EXPECT_EQ(updated->deployment_block, manifest.deployment_block);
        if(contract == publish::PtContract::REGISTRY)
        {
            EXPECT_EQ(updated->registry_impl, hexAddress(0xA0));
            EXPECT_EQ(updated->runner_impl, manifest.runner_impl);
        }
        else
        {
            EXPECT_EQ(updated->runner_impl, hexAddress(0xA0));
            EXPECT_EQ(updated->registry_impl, manifest.registry_impl);
        }

        // The second transaction is the owner's upgradeToAndCall(newImpl, "") on the proxy.
        ASSERT_EQ(node->sent_raw_transactions.size(), 2u);
        const auto upgrade = publish::decodeSignedTransaction(node->sent_raw_transactions[1]);
        ASSERT_TRUE(upgrade.has_value()) << (upgrade ? "" : upgrade.error());
        EXPECT_EQ(upgrade->sender, signer->address());
        ASSERT_TRUE(upgrade->transaction.to.has_value());
        EXPECT_EQ(chain::addressToHex(*upgrade->transaction.to), proxy);

        const auto & data = upgrade->transaction.data;
        const auto selector = chain::constructSelector("upgradeToAndCall(address,bytes)");
        ASSERT_GE(data.size(), 4u + 32u);
        EXPECT_TRUE(std::equal(selector.begin(), selector.end(), data.begin()));
        const auto implementation_word = encodeAddressWord(makeAddressFromByte(0xA0));
        EXPECT_TRUE(std::equal(implementation_word.begin(), implementation_word.end(), data.begin() + 4));
    }
}

TEST(PublishUpgrade, RefusesWhenTheDeploymentIsNotTheSignersOrHasDrifted)
{
    asio::io_context io_context;
    const auto signer = makeSigner();
    const auto paths = makeTempEventsPaths("publish_upgrade_refusals");
    const auto artifacts = writeDeployArtifacts(paths.root / "bin");

    auto node = makeDeployedNode(*signer);
    const publish::DeploymentManifest manifest = operatorManifest(*node, *signer);

    // Another account's deployment: nothing is sent.
    publish::DeploymentManifest foreign = manifest;
    foreign.owner = hexAddress(0x77);
    for(const auto contract : {publish::PtContract::REGISTRY, publish::PtContract::RUNNER})
    {
        SCOPED_TRACE(publish::contractLabel(contract));
        EXPECT_FALSE(runAwaitable(io_context, publish::upgradeContract(
            *makeClient(node), *signer, contract, implementationOf(artifacts, contract), foreign)).has_value());
    }

    // A manifest without a runner has none to upgrade.
    EXPECT_FALSE(runAwaitable(io_context, publish::upgradeContract(
        *makeClient(node), *signer, publish::PtContract::RUNNER, artifacts.runner_impl_bin,
        withoutRunner(manifest))).has_value());
    EXPECT_TRUE(node->sent_raw_transactions.empty());

    // A proxy that no longer matches the manifest is not upgraded, nor is the other one.
    node->implementation_by_proxy[RUNNER_ADDRESS] = wordOfAddress(hexAddress(0x01));
    for(const auto contract : {publish::PtContract::REGISTRY, publish::PtContract::RUNNER})
    {
        SCOPED_TRACE(publish::contractLabel(contract));
        EXPECT_FALSE(runAwaitable(io_context, publish::upgradeContract(
            *makeClient(node), *signer, contract, implementationOf(artifacts, contract), manifest)).has_value());
    }
    EXPECT_TRUE(node->sent_raw_transactions.empty());

    // An upgrade the proxy did not take fails verification instead of producing a manifest.
    node->implementation_by_proxy[RUNNER_ADDRESS] = wordOfAddress(RUNNER_IMPL_ADDRESS);
    EXPECT_FALSE(runAwaitable(io_context, publish::upgradeContract(
        *makeClient(node), *signer, publish::PtContract::RUNNER, artifacts.runner_impl_bin, manifest)).has_value());
    EXPECT_EQ(node->sent_raw_transactions.size(), 2u);
}

TEST(PublishUpgrade, RefusesToUpgradeUntilTheNewImplementationCodeCanBeRead)
{
    asio::io_context io_context;
    const auto signer = makeSigner();
    const auto paths = makeTempEventsPaths("publish_upgrade_code_preflight");
    const auto artifacts = writeDeployArtifacts(paths.root / "bin");
    auto node = makeDeployedNode(*signer);
    const auto manifest = operatorManifest(*node, *signer);
    node->on_send = [&node](const std::string &) { node->code_by_address[hexAddress(0xA0)] = "0x"; };

    const auto upgraded = runAwaitable(io_context, publish::upgradeContract(
        *makeClient(node), *signer, publish::PtContract::RUNNER, artifacts.runner_impl_bin, manifest));
    EXPECT_FALSE(upgraded.has_value());
    EXPECT_EQ(node->sent_raw_transactions.size(), 1u);
    EXPECT_EQ(node->implementation_by_proxy.at(RUNNER_ADDRESS), wordOfAddress(RUNNER_IMPL_ADDRESS));
}

TEST(PublishUpgrade, RefusesAnImplementationThatIsNotThisPtProtocolBeforeUpgrading)
{
    asio::io_context io_context;
    const auto signer = makeSigner();
    const auto paths = makeTempEventsPaths("publish_upgrade_identity_preflight");
    const auto artifacts = writeDeployArtifacts(paths.root / "bin");

    for(const auto contract : {publish::PtContract::REGISTRY, publish::PtContract::RUNNER})
    {
        SCOPED_TRACE(publish::contractLabel(contract));
        auto node = makeDeployedNode(*signer);
        const auto manifest = operatorManifest(*node, *signer);

        // The newly deployed implementation answers another protocol version.
        node->call_override = [](const json & request) -> std::string
        {
            const json & call = request.at("params").at(0);
            if(chain::normalizeHex(call.value("to", "")) == hexAddress(0xA0)
               && call.value("data", "").starts_with(selectorOf("protocolVersion()")))
            {
                return rpcResult(request, wordOfUint(rpc::PT_PROTOCOL_VERSION + 1));
            }
            return {};
        };

        const auto upgraded = runAwaitable(io_context, publish::upgradeContract(
            *makeClient(node), *signer, contract, implementationOf(artifacts, contract), manifest));
        ASSERT_FALSE(upgraded.has_value());
        EXPECT_NE(upgraded.error().find("the proxy was not upgraded"), std::string::npos);
        // Only the implementation was deployed; no upgradeToAndCall was sent.
        EXPECT_EQ(node->sent_raw_transactions.size(), 1u);
        EXPECT_EQ(node->implementation_by_proxy.at(proxyOf(contract)),
            wordOfAddress(contract == publish::PtContract::REGISTRY ? REGISTRY_IMPL_ADDRESS : RUNNER_IMPL_ADDRESS));
    }
}

TEST(PublishUpgrade, FailedPostUpgradeVerificationIncludesManifestRecoveryValues)
{
    for(const auto contract : {publish::PtContract::REGISTRY, publish::PtContract::RUNNER})
    {
        const std::string label(publish::contractLabel(contract));
        SCOPED_TRACE(label);
        asio::io_context io_context;
        const auto signer = makeSigner();
        const auto paths = makeTempEventsPaths(std::format("publish_upgrade_recovery_{}", label));
        const auto artifacts = writeDeployArtifacts(paths.root / "bin");
        auto node = makeDeployedNode(*signer);
        const auto manifest = operatorManifest(*node, *signer);
        const std::string proxy = proxyOf(contract);
        const std::vector<std::uint8_t> new_code{0x60, 0x01};
        const std::string new_code_hash = chain::bytes32ToHex(chain::keccakBytes(new_code));
        node->code_by_address[hexAddress(0xA0)] = chain::bytesToHex(new_code);
        node->on_send = [&node, proxy](const std::string &)
        {
            if(node->sent_raw_transactions.size() == 2)
            {
                node->implementation_by_proxy[proxy] = wordOfAddress(hexAddress(0xA0));
                node->call_override = [](const json & request)
                {
                    return rpcError(request, -32005, "rate limit exceeded");
                };
            }
        };

        const auto upgraded = runAwaitable(io_context, publish::upgradeContract(
            *makeClient(node), *signer, contract, implementationOf(artifacts, contract), manifest));
        ASSERT_FALSE(upgraded.has_value());
        ASSERT_EQ(node->sent_raw_transactions.size(), 2u);
        EXPECT_NE(upgraded.error().find(publish::transactionHash(node->sent_raw_transactions.back())), std::string::npos);
        EXPECT_NE(upgraded.error().find(label + "_impl=" + hexAddress(0xA0)), std::string::npos);
        EXPECT_NE(upgraded.error().find(label + "_impl_code_hash=" + new_code_hash), std::string::npos);

        // The reported values restore a manifest that verifies after the provider recovers.
        node->call_override = {};
        auto recovered = manifest;
        if(contract == publish::PtContract::REGISTRY)
        {
            recovered.registry_impl = hexAddress(0xA0);
            recovered.registry_impl_code_hash = new_code_hash;
        }
        else
        {
            recovered.runner_impl = hexAddress(0xA0);
            recovered.runner_impl_code_hash = new_code_hash;
        }
        EXPECT_TRUE(runAwaitable(io_context, publish::verifyDeployment(*makeClient(node), recovered)).has_value());
    }
}
