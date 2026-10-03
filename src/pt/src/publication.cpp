#include <algorithm>
#include <cstring>
#include <format>
#include <limits>
#include <tuple>
#include <utility>

#include <nlohmann/json.hpp>
#include <secp256k1.h>
#include <secp256k1_recovery.h>

#include "crypto.hpp"
#include "parser.hpp"

#include "publication.hpp"

namespace dcn::pt::abi
{
    namespace
    {
        constexpr std::size_t WORD = 32;

        std::vector<std::uint8_t> zeroWord()
        {
            return std::vector<std::uint8_t>(WORD, 0);
        }

        std::vector<std::uint8_t> uintWord(const std::uint64_t value)
        {
            std::vector<std::uint8_t> word = zeroWord();
            for(std::size_t i = 0; i < 8; ++i)
            {
                word[WORD - 1 - i] = static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFu);
            }
            return word;
        }

        std::vector<std::uint8_t> int32Word(const std::int32_t value)
        {
            // Two's-complement sign extension to 32 bytes.
            const std::uint8_t fill = (value < 0) ? 0xFFu : 0x00u;
            std::vector<std::uint8_t> word(WORD, fill);
            const std::uint32_t bits = static_cast<std::uint32_t>(value);
            for(std::size_t i = 0; i < 4; ++i)
            {
                word[WORD - 1 - i] = static_cast<std::uint8_t>((bits >> (8 * i)) & 0xFFu);
            }
            return word;
        }

        void appendPadded(std::vector<std::uint8_t> & out, std::span<const std::uint8_t> bytes)
        {
            out.insert(out.end(), bytes.begin(), bytes.end());
            const std::size_t padding = (WORD - (bytes.size() % WORD)) % WORD;
            out.insert(out.end(), padding, 0);
        }
    }

    Component staticWord(std::vector<std::uint8_t> word)
    {
        return Component{.dynamic = false, .data = std::move(word)};
    }

    Component uintComponent(const std::uint64_t value)
    {
        return staticWord(uintWord(value));
    }

    Component addressComponent(const chain::Address & value)
    {
        std::vector<std::uint8_t> word = zeroWord();
        std::memcpy(word.data() + 12, value.bytes, 20);
        return staticWord(std::move(word));
    }

    Component bytes32Component(const evmc::bytes32 & value)
    {
        std::vector<std::uint8_t> word(value.bytes, value.bytes + WORD);
        return staticWord(std::move(word));
    }

    Component bytesComponent(std::span<const std::uint8_t> value)
    {
        std::vector<std::uint8_t> out = uintWord(value.size());
        appendPadded(out, value);
        return Component{.dynamic = true, .data = std::move(out)};
    }

    Component stringComponent(const std::string & value)
    {
        return bytesComponent(std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t *>(value.data()), value.size()));
    }

    Component uint32ArrayComponent(const std::vector<std::uint32_t> & values)
    {
        std::vector<std::uint8_t> out = uintWord(values.size());
        for(const std::uint32_t value : values)
        {
            const std::vector<std::uint8_t> word = uintWord(value);
            out.insert(out.end(), word.begin(), word.end());
        }
        return Component{.dynamic = true, .data = std::move(out)};
    }

    Component int32ArrayComponent(const std::vector<std::int32_t> & values)
    {
        std::vector<std::uint8_t> out = uintWord(values.size());
        for(const std::int32_t value : values)
        {
            const std::vector<std::uint8_t> word = int32Word(value);
            out.insert(out.end(), word.begin(), word.end());
        }
        return Component{.dynamic = true, .data = std::move(out)};
    }

    Component stringArrayComponent(const std::vector<std::string> & values)
    {
        std::vector<Component> elements;
        elements.reserve(values.size());
        for(const std::string & value : values)
        {
            elements.push_back(stringComponent(value));
        }

        std::vector<std::uint8_t> out = uintWord(values.size());
        const std::vector<std::uint8_t> body = encodeTuple(elements);
        out.insert(out.end(), body.begin(), body.end());
        return Component{.dynamic = true, .data = std::move(out)};
    }

    std::vector<std::uint8_t> encodeTuple(const std::vector<Component> & components)
    {
        const std::size_t tail_offset = components.size() * WORD;

        std::vector<std::uint8_t> head;
        std::vector<std::uint8_t> tail;
        head.reserve(tail_offset);

        for(const Component & component : components)
        {
            if(!component.dynamic)
            {
                head.insert(head.end(), component.data.begin(), component.data.end());
                continue;
            }

            const std::vector<std::uint8_t> offset = uintWord(tail_offset + tail.size());
            head.insert(head.end(), offset.begin(), offset.end());
            tail.insert(tail.end(), component.data.begin(), component.data.end());
        }

        head.insert(head.end(), tail.begin(), tail.end());
        return head;
    }
}

namespace dcn::pt
{
    namespace
    {
        const std::string PUBLICATION_TUPLE =
            "(uint8,string,address,bytes32,bytes32,bytes32,bytes32,bytes32,uint256,uint256)";

        const std::string CONNECTOR_REGISTRATION_TUPLE =
            "(uint32,uint32[],string[],uint32[],uint32[],string[],string,int32[],"
            "uint32[],uint32[],uint32[],uint32[],string[],uint32[],int32[])";

        abi::Component publicationComponent(const Publication & publication)
        {
            const std::vector<abi::Component> fields{
                abi::uintComponent(static_cast<std::uint64_t>(publication.kind)),
                abi::stringComponent(publication.name),
                abi::addressComponent(publication.owner),
                abi::bytes32Component(publication.content_hash),
                abi::bytes32Component(publication.metadata_hash),
                abi::bytes32Component(publication.creation_code_hash),
                abi::bytes32Component(publication.runtime_code_hash),
                abi::bytes32Component(publication.registration_hash),
                abi::uintComponent(publication.nonce),
                abi::uintComponent(publication.deadline)
            };

            // The tuple holds a string, so it encodes as a dynamic component.
            return abi::Component{.dynamic = true, .data = abi::encodeTuple(fields)};
        }

        abi::Component registrationComponent(const ConnectorRegistration & registration)
        {
            const std::vector<abi::Component> fields{
                abi::uintComponent(registration.dimensions_count),
                abi::uint32ArrayComponent(registration.composite_dim_ids),
                abi::stringArrayComponent(registration.composite_names),
                abi::uint32ArrayComponent(registration.binding_dim_ids),
                abi::uint32ArrayComponent(registration.binding_slot_ids),
                abi::stringArrayComponent(registration.binding_names),
                abi::stringComponent(registration.condition_name),
                abi::int32ArrayComponent(registration.condition_args),
                abi::uint32ArrayComponent(registration.static_ri_positions),
                abi::uint32ArrayComponent(registration.static_ri_start_points),
                abi::uint32ArrayComponent(registration.static_ri_transform_shifts),
                abi::uint32ArrayComponent(registration.transformation_dim_ids),
                abi::stringArrayComponent(registration.transformation_names),
                abi::uint32ArrayComponent(registration.transformation_arg_counts),
                abi::int32ArrayComponent(registration.transformation_args)
            };

            return abi::Component{.dynamic = true, .data = abi::encodeTuple(fields)};
        }

        std::string publishSignature(const EntityKind kind)
        {
            switch(kind)
            {
            case EntityKind::TRANSFORMATION:
                return std::format("publishTransformation(bytes,{},bytes)", PUBLICATION_TUPLE);
            case EntityKind::CONDITION:
                return std::format("publishCondition(bytes,{},bytes)", PUBLICATION_TUPLE);
            case EntityKind::CONNECTOR:
                return std::format("publishConnector(bytes,{},{},bytes)", PUBLICATION_TUPLE, CONNECTOR_REGISTRATION_TUPLE);
            }
            return {};
        }

        std::vector<std::uint8_t> encodeCall(
            const std::string & signature,
            const std::vector<abi::Component> & arguments)
        {
            std::vector<std::uint8_t> out = chain::constructSelector(signature);
            const std::vector<std::uint8_t> body = abi::encodeTuple(arguments);
            out.insert(out.end(), body.begin(), body.end());
            return out;
        }
    }

    std::optional<ConnectorRegistration> buildConnectorRegistration(const Connector & connector)
    {
        ConnectorRegistration registration;
        registration.dimensions_count = static_cast<std::uint32_t>(connector.dimensions_size());
        registration.condition_name = connector.condition_name();

        for(int i = 0; i < connector.condition_args_size(); ++i)
        {
            registration.condition_args.push_back(connector.condition_args(i));
        }

        for(int dim_index = 0; dim_index < connector.dimensions_size(); ++dim_index)
        {
            const std::uint32_t dim_id = static_cast<std::uint32_t>(dim_index);
            const Dimension & dimension = connector.dimensions(dim_index);

            if(!dimension.composite().empty())
            {
                registration.composite_dim_ids.push_back(dim_id);
                registration.composite_names.push_back(dimension.composite());
            }

            // Protobuf map iteration order is unspecified; the emitted arrays must be
            // deterministic so the event decodes back to the same record.
            std::vector<std::pair<std::uint32_t, std::string>> bindings;
            bindings.reserve(static_cast<std::size_t>(dimension.bindings_size()));
            for(const auto & [slot, target] : dimension.bindings())
            {
                const auto slot_id = parse::parseUint32Decimal(slot);
                if(!slot_id)
                {
                    return std::nullopt;
                }
                bindings.emplace_back(*slot_id, target);
            }
            std::ranges::sort(bindings, [](const auto & lhs, const auto & rhs)
            {
                return lhs.first < rhs.first;
            });

            for(const auto & [slot_id, target] : bindings)
            {
                registration.binding_dim_ids.push_back(dim_id);
                registration.binding_slot_ids.push_back(slot_id);
                registration.binding_names.push_back(target);
            }

            for(int op_id = 0; op_id < dimension.transformations_size(); ++op_id)
            {
                const TransformationDef & transformation = dimension.transformations(op_id);
                registration.transformation_dim_ids.push_back(dim_id);
                registration.transformation_names.push_back(transformation.name());
                registration.transformation_arg_counts.push_back(
                    static_cast<std::uint32_t>(transformation.args_size()));
                for(int arg_id = 0; arg_id < transformation.args_size(); ++arg_id)
                {
                    registration.transformation_args.push_back(transformation.args(arg_id));
                }
            }
        }

        std::vector<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>> static_ri;
        static_ri.reserve(static_cast<std::size_t>(connector.static_ri_size()));
        for(const auto & [position, running_instance] : connector.static_ri())
        {
            static_ri.emplace_back(position, running_instance.start_point(), running_instance.transformation_shift());
        }
        std::ranges::sort(static_ri, [](const auto & lhs, const auto & rhs)
        {
            return std::get<0>(lhs) < std::get<0>(rhs);
        });

        for(const auto & [position, start_point, transform_shift] : static_ri)
        {
            registration.static_ri_positions.push_back(position);
            registration.static_ri_start_points.push_back(start_point);
            registration.static_ri_transform_shifts.push_back(transform_shift);
        }

        return registration;
    }

    evmc::bytes32 computeRegistrationHash(const ConnectorRegistration & registration)
    {
        // abi.encode of one struct argument: the offset word, then the tuple.
        const std::vector<std::uint8_t> encoded = abi::encodeTuple({registrationComponent(registration)});
        return chain::keccakBytes(encoded);
    }

    evmc::bytes32 computeContentHash(const std::string & artifact_bytes)
    {
        return chain::keccakString(artifact_bytes);
    }

    evmc::bytes32 computeMetadataHash(const EntityKind kind, const std::string & name)
    {
        const std::string canonical = parse::dumpJsonLossy(nlohmann::json{
            {"entity_type", std::string(entityKindName(kind))},
            {"name", name},
            {"schema_version", 1}
        });
        return chain::keccakString(canonical);
    }

    chain::Address predictEntityAddress(
        const chain::Address & registry_address,
        const Publication & publication,
        const std::span<const std::uint8_t> creation_code)
    {
        const std::vector<std::uint8_t> salt_preimage = abi::encodeTuple({
            abi::addressComponent(registry_address),
            abi::uintComponent(static_cast<std::uint64_t>(publication.kind)),
            abi::bytes32Component(chain::keccakString(publication.name)),
            abi::addressComponent(publication.owner),
            abi::bytes32Component(publication.content_hash),
            abi::uintComponent(publication.nonce)});
        const evmc::bytes32 salt = chain::keccakBytes(salt_preimage);
        const evmc::bytes32 code_hash = chain::keccakBytes(creation_code);

        std::vector<std::uint8_t> preimage{0xFF};
        preimage.insert(preimage.end(), registry_address.bytes, registry_address.bytes + 20);
        preimage.insert(preimage.end(), salt.bytes, salt.bytes + 32);
        preimage.insert(preimage.end(), code_hash.bytes, code_hash.bytes + 32);

        const evmc::bytes32 digest = chain::keccakBytes(preimage);
        chain::Address address{};
        std::copy(digest.bytes + 12, digest.bytes + 32, address.bytes);
        return address;
    }

    std::vector<std::uint8_t> encodePublishTransformationCall(
        std::span<const std::uint8_t> creation_code,
        const Publication & publication,
        std::span<const std::uint8_t> owner_signature)
    {
        return encodeCall(
            publishSignature(EntityKind::TRANSFORMATION),
            {abi::bytesComponent(creation_code),
             publicationComponent(publication),
             abi::bytesComponent(owner_signature)});
    }

    std::vector<std::uint8_t> encodePublishConditionCall(
        std::span<const std::uint8_t> creation_code,
        const Publication & publication,
        std::span<const std::uint8_t> owner_signature)
    {
        return encodeCall(
            publishSignature(EntityKind::CONDITION),
            {abi::bytesComponent(creation_code),
             publicationComponent(publication),
             abi::bytesComponent(owner_signature)});
    }

    std::vector<std::uint8_t> encodePublishConnectorCall(
        std::span<const std::uint8_t> creation_code,
        const Publication & publication,
        const ConnectorRegistration & registration,
        std::span<const std::uint8_t> owner_signature)
    {
        return encodeCall(
            publishSignature(EntityKind::CONNECTOR),
            {abi::bytesComponent(creation_code),
             publicationComponent(publication),
             registrationComponent(registration),
             abi::bytesComponent(owner_signature)});
    }

    std::map<chain::Address, std::vector<std::uint8_t>> findPublishedCreationCodes(
        const std::span<const std::uint8_t> calldata,
        const chain::Address & registry)
    {
        std::vector<std::vector<std::uint8_t>> selectors;
        for(const EntityKind kind : {EntityKind::TRANSFORMATION, EntityKind::CONDITION, EntityKind::CONNECTOR})
        {
            selectors.push_back(chain::constructSelector(publishSignature(kind)));
        }

        std::map<chain::Address, std::vector<std::uint8_t>> codes;
        for(std::size_t offset = 0; offset + 4 <= calldata.size(); ++offset)
        {
            const auto at = calldata.begin() + static_cast<std::ptrdiff_t>(offset);
            if(std::none_of(selectors.begin(), selectors.end(),
                   [&](const auto & selector) { return std::equal(selector.begin(), selector.end(), at); }))
            {
                continue;
            }

            const std::span<const std::uint8_t> call = calldata.subspan(offset);
            const auto publication = decodePublishCall(call);
            if(!publication) continue;

            // The creation code is the first argument; an ABI bytes value is encoded as a string.
            const std::uint8_t * arguments = call.data() + 4;
            const std::size_t arguments_size = call.size() - 4;
            const auto code_offset = chain::readWordAsSizeT(arguments, arguments_size, 0);
            const auto code = code_offset ? chain::decodeAbiString(arguments, arguments_size, *code_offset) : std::nullopt;
            if(!code || code->empty()) continue;

            std::vector<std::uint8_t> creation_code(code->begin(), code->end());
            const chain::Address address = predictEntityAddress(registry, *publication, creation_code);
            codes.try_emplace(address, std::move(creation_code));
        }
        return codes;
    }

    std::optional<Publication> decodePublishCall(const std::span<const std::uint8_t> calldata)
    {
        if(calldata.size() < 4) return std::nullopt;

        std::optional<EntityKind> kind;
        for(const EntityKind candidate : {EntityKind::TRANSFORMATION, EntityKind::CONDITION, EntityKind::CONNECTOR})
        {
            const std::vector<std::uint8_t> selector = chain::constructSelector(publishSignature(candidate));
            if(std::equal(selector.begin(), selector.end(), calldata.begin()))
            {
                kind = candidate;
                break;
            }
        }
        if(!kind) return std::nullopt;

        const std::uint8_t * arguments = calldata.data() + 4;
        const std::size_t arguments_size = calldata.size() - 4;

        // The publication tuple is the second argument of every publish call.
        const auto tuple_offset = chain::readWordAsSizeT(arguments, arguments_size, 32);
        if(!tuple_offset || *tuple_offset > arguments_size) return std::nullopt;
        const std::uint8_t * tuple = arguments + *tuple_offset;
        const std::size_t tuple_size = arguments_size - *tuple_offset;

        const auto word = [&](const std::size_t index)
        {
            return chain::readWordAsSizeT(tuple, tuple_size, index * 32);
        };
        const auto bytes32 = [&](const std::size_t index) -> std::optional<evmc::bytes32>
        {
            if(tuple_size < (index + 1) * 32) return std::nullopt;
            evmc::bytes32 out{};
            std::copy_n(tuple + index * 32, 32, out.bytes);
            return out;
        };

        const auto encoded_kind = word(0);
        if(!encoded_kind || *encoded_kind != static_cast<std::size_t>(*kind)) return std::nullopt;

        const auto name_offset = word(1);
        if(!name_offset) return std::nullopt;
        auto name = chain::decodeAbiString(tuple, tuple_size, *name_offset);

        // An address word is left-padded with twelve zero bytes.
        const auto owner_word = bytes32(2);
        if(!owner_word || std::any_of(owner_word->bytes, owner_word->bytes + 12, [](const std::uint8_t b) { return b != 0; }))
        {
            return std::nullopt;
        }

        const auto content_hash = bytes32(3);
        const auto metadata_hash = bytes32(4);
        const auto creation_code_hash = bytes32(5);
        const auto runtime_code_hash = bytes32(6);
        const auto registration_hash = bytes32(7);
        const auto nonce = word(8);
        // The deadline is a uint256: a value beyond 64 bits never expires.
        const auto deadline = tuple_size >= 10 * 32
            ? std::optional(chain::readUint64Word(tuple, tuple_size, 9 * 32).value_or(std::numeric_limits<std::uint64_t>::max()))
            : std::nullopt;
        if(!name || !content_hash || !metadata_hash || !creation_code_hash || !runtime_code_hash
           || !registration_hash || !nonce || !deadline)
        {
            return std::nullopt;
        }

        Publication publication;
        publication.kind = *kind;
        publication.name = std::move(*name);
        std::copy_n(owner_word->bytes + 12, 20, publication.owner.bytes);
        publication.content_hash = *content_hash;
        publication.metadata_hash = *metadata_hash;
        publication.creation_code_hash = *creation_code_hash;
        publication.runtime_code_hash = *runtime_code_hash;
        publication.registration_hash = *registration_hash;
        publication.nonce = static_cast<std::uint64_t>(*nonce);
        publication.deadline = *deadline;
        return publication;
    }

    std::vector<std::uint8_t> encodePublicationNonceCall(const chain::Address & owner)
    {
        std::vector<std::uint8_t> out = chain::constructSelector("publicationNonce(address)");
        const abi::Component component = abi::addressComponent(owner);
        out.insert(out.end(), component.data.begin(), component.data.end());
        return out;
    }

    evmc::bytes32 publicationDigest(
        const std::uint64_t chain_id,
        const chain::Address & registry_address,
        const Publication & publication)
    {
        static const std::string DOMAIN_TYPE =
            "EIP712Domain(string name,string version,uint256 chainId,address verifyingContract)";
        static const std::string PUBLICATION_TYPE =
            "Publication(uint8 kind,string name,address owner,bytes32 contentHash,bytes32 metadataHash,"
            "bytes32 creationCodeHash,bytes32 runtimeCodeHash,bytes32 registrationHash,uint256 nonce,uint256 deadline)";

        const auto hashWord = [](const std::string & text)
        {
            return abi::bytes32Component(chain::keccakBytes(std::span<const std::uint8_t>(
                reinterpret_cast<const std::uint8_t *>(text.data()), text.size())));
        };
        const auto concatWords = [](std::initializer_list<abi::Component> components)
        {
            std::vector<std::uint8_t> out;
            out.reserve(components.size() * 32);
            for(const abi::Component & component : components)
            {
                out.insert(out.end(), component.data.begin(), component.data.end());
            }
            return out;
        };

        const evmc::bytes32 domain_separator = chain::keccakBytes(concatWords({
            hashWord(DOMAIN_TYPE),
            hashWord("PT"),
            hashWord("1"),
            abi::uintComponent(chain_id),
            abi::addressComponent(registry_address)}));

        const evmc::bytes32 struct_hash = chain::keccakBytes(concatWords({
            hashWord(PUBLICATION_TYPE),
            abi::uintComponent(static_cast<std::uint64_t>(publication.kind)),
            hashWord(publication.name),
            abi::addressComponent(publication.owner),
            abi::bytes32Component(publication.content_hash),
            abi::bytes32Component(publication.metadata_hash),
            abi::bytes32Component(publication.creation_code_hash),
            abi::bytes32Component(publication.runtime_code_hash),
            abi::bytes32Component(publication.registration_hash),
            abi::uintComponent(publication.nonce),
            abi::uintComponent(publication.deadline)}));

        // keccak256(0x1901 || domainSeparator || structHash)
        std::vector<std::uint8_t> preimage{0x19, 0x01};
        preimage.insert(preimage.end(), domain_separator.bytes, domain_separator.bytes + 32);
        preimage.insert(preimage.end(), struct_hash.bytes, struct_hash.bytes + 32);

        return chain::keccakBytes(preimage);
    }

    std::optional<chain::Address> recoverSigner(
        const evmc::bytes32 & digest,
        const std::span<const std::uint8_t> signature)
    {
        if(signature.size() != 65) return std::nullopt;

        // Wallets emit v as 27/28; a raw 0/1 recovery id is also accepted.
        int recovery_id = static_cast<int>(signature[64]);
        if(recovery_id >= 27) recovery_id -= 27;
        if(recovery_id != 0 && recovery_id != 1) return std::nullopt;

        // Recovery involves no secret key, so the static context is sufficient.
        const secp256k1_context * ctx = secp256k1_context_static;

        secp256k1_ecdsa_recoverable_signature parsed;
        if(!secp256k1_ecdsa_recoverable_signature_parse_compact(ctx, &parsed, signature.data(), recovery_id))
        {
            return std::nullopt;
        }

        secp256k1_ecdsa_signature normalized;
        secp256k1_ecdsa_recoverable_signature_convert(ctx, &normalized, &parsed);
        if(secp256k1_ecdsa_signature_normalize(ctx, nullptr, &normalized) != 0)
        {
            return std::nullopt;
        }

        secp256k1_pubkey pubkey;
        if(!secp256k1_ecdsa_recover(ctx, &pubkey, &parsed, digest.bytes))
        {
            return std::nullopt;
        }

        std::array<std::uint8_t, 65> serialized{};
        std::size_t length = serialized.size();
        secp256k1_ec_pubkey_serialize(ctx, serialized.data(), &length, &pubkey, SECP256K1_EC_UNCOMPRESSED);
        return chain::ethAddressFromPublicKey(serialized.data(), length);
    }
}
