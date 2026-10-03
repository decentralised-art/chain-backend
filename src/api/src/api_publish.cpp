#include "api.hpp"
#include "address.hpp"
#include "publish.hpp"

namespace dcn
{
    namespace
    {
        http::Response makeResponse()
        {
            http::Response response;
            response.setCode(http::Code::Unknown)
                    .setVersion("HTTP/1.1")
                    .setHeader(http::Header::AccessControlAllowOrigin, "*")
                    .setHeader(http::Header::ContentType, "application/json")
                    .setHeader(http::Header::CacheControl, "no-store")
                    .setHeader(http::Header::Connection, "close");
            return response;
        }

        http::Response failure(const http::Code code, const std::string & message)
        {
            http::Response response = makeResponse();
            response.setCode(code).setBodyWithContentLength(json{{"message", message}}.dump());
            return response;
        }

        http::Response publishFailure(const publish::PublishError & error)
        {
            http::Code code = http::Code::ServiceUnavailable;
            switch(error.kind)
            {
            case publish::PublishError::Kind::INVALID: code = http::Code::BadRequest; break;
            case publish::PublishError::Kind::CONFLICT: code = http::Code::Conflict; break;
            case publish::PublishError::Kind::REVERTED: code = http::Code::BadRequest; break;
            case publish::PublishError::Kind::FORBIDDEN: code = http::Code::Forbidden; break;
            case publish::PublishError::Kind::PENDING: code = http::Code::Accepted; break;
            case publish::PublishError::Kind::UPSTREAM: code = http::Code::ServiceUnavailable; break;
            }

            json body{{"message", error.message}};
            if(error.kind == publish::PublishError::Kind::PENDING) body["status"] = "pending";
            if(!error.tx_hash.empty()) body["tx_hash"] = error.tx_hash;
            if(!error.missing.empty()) body["missing"] = error.missing;
            if(!error.mismatched.empty()) body["mismatched"] = error.mismatched;

            http::Response response = makeResponse();
            response.setCode(code).setBodyWithContentLength(body.dump());
            return response;
        }

        std::string quantity(const std::uint64_t value)
        {
            return std::format("0x{:x}", value);
        }

        std::expected<std::string, http::Response> stringField(const json & body, const char * field)
        {
            if(!body.contains(field) || !body.at(field).is_string())
            {
                return std::unexpected(failure(http::Code::BadRequest, std::format("'{}' is required", field)));
            }
            return body.at(field).get<std::string>();
        }

        struct PublishTarget
        {
            pt::EntityKind kind = pt::EntityKind::TRANSFORMATION;
            std::string name;
            chain::Address owner{};
            json body;
        };

        /// The route kind, the authenticated owner and the entity name every /publish
        /// request carries.
        asio::awaitable<std::expected<PublishTarget, http::Response>> beginPublication(
            const http::Request & request,
            const std::vector<server::RouteArg> & args,
            const auth::AuthManager & auth_manager,
            const publish::Publisher * publisher)
        {
            if(publisher == nullptr)
            {
                co_return std::unexpected(failure(http::Code::Forbidden, "Publication is disabled on this server"));
            }

            if(args.size() != 1)
            {
                co_return std::unexpected(failure(http::Code::BadRequest, "Expected /publish/<kind>"));
            }
            const auto kind_name = parse::parseRouteArgAs<std::string>(args.at(0));
            const auto kind = kind_name ? pt::parseEntityKind(*kind_name) : std::nullopt;
            if(!kind)
            {
                co_return std::unexpected(failure(
                    http::Code::NotFound, "Kind must be one of transformation, condition, connector"));
            }

            const auto auth_result = co_await authenticate(request, auth_manager);
            if(!auth_result)
            {
                co_return std::unexpected(failure(
                    http::Code::Unauthorized, std::format("Authentication error: {}", auth_result.error().kind)));
            }

            PublishTarget target{.kind = *kind, .owner = *auth_result};
            target.body = json::parse(request.getBody(), nullptr, false);
            if(target.body.is_discarded() || !target.body.is_object())
            {
                co_return std::unexpected(failure(http::Code::BadRequest, "Request body is not a JSON object"));
            }

            auto name = stringField(target.body, "name");
            if(!name) co_return std::unexpected(std::move(name.error()));
            if(!pt::isValidEntityName(*name))
            {
                co_return std::unexpected(failure(http::Code::BadRequest, "'name' is not a valid entity name"));
            }
            target.name = std::move(*name);

            co_return target;
        }

        /// The publication a /publish/<kind> or /publish/<kind>/send request names, from
        /// the route kind, the caller and the body's name and content hash.
        std::expected<publish::PublicationClaim, http::Response> claimOf(const PublishTarget & target)
        {
            auto content_hash_field = stringField(target.body, "content_hash");
            if(!content_hash_field) return std::unexpected(std::move(content_hash_field.error()));
            // Exactly 32 bytes: a typed parse would left-pad a short value into a valid hash.
            const auto content_hash = evmc::from_hex(*content_hash_field);
            if(!content_hash || content_hash->size() != 32)
            {
                return std::unexpected(failure(http::Code::BadRequest, "'content_hash' must be a 32-byte hex value"));
            }

            return publish::PublicationClaim{
                .kind = target.kind,
                .name = target.name,
                .owner = target.owner,
                .content_hash = chain::bytesToHex(*content_hash),
                .metadata_hash = chain::bytes32ToHex(pt::computeMetadataHash(target.kind, target.name))};
        }

        /// The retained local definition of an entity the caller owns.
        template<class Record>
        std::expected<Record, http::Response> ownedLocalRecord(
            std::optional<Record> record,
            const chain::Address & owner)
        {
            if(!record)
            {
                return std::unexpected(failure(http::Code::NotFound, "No local definition of this entity exists"));
            }
            const auto record_owner = evmc::from_hex<chain::Address>(record->owner());
            if(!record_owner || *record_owner != owner)
            {
                return std::unexpected(failure(http::Code::Forbidden, "Only the entity owner can publish it"));
            }
            return std::move(*record);
        }

        /// Rebuilds the entity from its retained local definition: the registry mirrors
        /// chain-derivable state only and never holds the Solidity source a build needs.
        asio::awaitable<std::expected<loader::PreparedPublication, http::Response>> rebuild(
            const PublishTarget & target,
            evm::EVM & evm,
            const publish::Publisher & publisher,
            const std::filesystem::path & storage_path)
        {
            std::expected<loader::PreparedPublication, pt::PTDeployError> prepared =
                std::unexpected(pt::PTDeployError{});
            switch(target.kind)
            {
            case pt::EntityKind::TRANSFORMATION:
            {
                auto record = ownedLocalRecord(loader::loadLocalTransformation(storage_path, target.name), target.owner);
                if(!record) co_return std::unexpected(std::move(record.error()));
                prepared = co_await loader::prepareTransformationPublication(evm, std::move(*record), storage_path);
                break;
            }
            case pt::EntityKind::CONDITION:
            {
                auto record = ownedLocalRecord(loader::loadLocalCondition(storage_path, target.name), target.owner);
                if(!record) co_return std::unexpected(std::move(record.error()));
                prepared = co_await loader::prepareConditionPublication(evm, std::move(*record), storage_path);
                break;
            }
            case pt::EntityKind::CONNECTOR:
            {
                auto record = ownedLocalRecord(loader::loadLocalConnector(storage_path, target.name), target.owner);
                if(!record) co_return std::unexpected(std::move(record.error()));
                prepared = co_await loader::prepareConnectorPublication(
                    evm, std::move(*record), storage_path, publisher.registryAddress());
                break;
            }
            }

            if(!prepared)
            {
                co_return std::unexpected(failure(http::Code::BadRequest, std::format(
                    "Cannot rebuild the publication: {}", prepared.error().kind)));
            }
            co_return std::move(*prepared);
        }
    }

    asio::awaitable<http::Response> OPTIONS_publish(
        const http::Request &,
        std::vector<server::RouteArg>,
        server::QueryArgsList)
    {
        http::Response response;
        response.setCode(http::Code::NoContent)
                .setVersion("HTTP/1.1")
                .setHeader(http::Header::AccessControlAllowOrigin, "*")
                .setHeader(http::Header::AccessControlAllowMethods, "POST, OPTIONS")
                .setHeader(http::Header::AccessControlAllowHeaders, "Authorization, Content-Type")
                .setHeader(http::Header::AccessControlMaxAge, "600")
                .setHeader(http::Header::Connection, "close");

        co_return response;
    }

    asio::awaitable<http::Response> POST_publishPrepare(
        const http::Request & request,
        std::vector<server::RouteArg> args,
        server::QueryArgsList,
        auth::AuthManager & auth_manager,
        evm::EVM & evm,
        const publish::Publisher * publisher,
        const config::Config & config)
    {
        auto target = co_await beginPublication(request, args, auth_manager, publisher);
        if(!target) co_return std::move(target.error());

        bool relay = false;
        if(target->body.contains("relay"))
        {
            if(!target->body.at("relay").is_boolean())
            {
                co_return failure(http::Code::BadRequest, "'relay' must be a boolean");
            }
            relay = target->body.at("relay").get<bool>();
        }

        auto prepared = co_await rebuild(*target, evm, *publisher, config.storage_path);
        if(!prepared) co_return std::move(prepared.error());

        const std::string kind_name(pt::entityKindName(target->kind));

        const auto existing = co_await publisher->findPublished(prepared->publication);
        if(!existing) co_return publishFailure(existing.error());
        if(*existing)
        {
            http::Response response = makeResponse();
            response.setCode(http::Code::OK).setBodyWithContentLength(json{
                {"status", "published"},
                {"kind", kind_name},
                {"name", target->name},
                {"address", (*existing)->entity_address},
                {"owner", (*existing)->owner},
                {"content_hash", (*existing)->content_hash}
            }.dump());
            co_return response;
        }

        const auto transaction = co_await publisher->prepare(publish::PublicationRequest{
            .publication = std::move(prepared->publication),
            .creation_code = std::move(prepared->creation_code),
            .registration = std::move(prepared->registration),
            .dependencies = std::move(prepared->dependencies)},
            relay);
        if(!transaction) co_return publishFailure(transaction.error());

        json body{
            {"status", "prepared"},
            {"kind", kind_name},
            {"name", target->name},
            // Ready for eth_sendTransaction: quantities are hex, as wallets expect.
            {"transaction", {
                {"from", transaction->from},
                {"to", transaction->to},
                {"data", transaction->data},
                {"chainId", quantity(transaction->chain_id)},
                {"gas", quantity(transaction->gas)}
            }},
            {"address", transaction->entity_address},
            {"content_hash", transaction->content_hash},
            {"publication_nonce", transaction->publication_nonce},
            {"deadline", transaction->deadline}
        };
        if(transaction->signing)
        {
            // Merged into `transaction`, these make it signable offline for /send.
            body["signing"] = {
                {"type", "0x2"},
                {"nonce", quantity(transaction->signing->nonce)},
                {"maxFeePerGas", quantity(transaction->signing->max_fee_per_gas)},
                {"maxPriorityFeePerGas", quantity(transaction->signing->max_priority_fee_per_gas)},
                {"value", "0x0"}
            };
        }

        http::Response response = makeResponse();
        response.setCode(http::Code::OK).setBodyWithContentLength(body.dump());
        co_return response;
    }

    asio::awaitable<http::Response> POST_publishSend(
        const http::Request & request,
        std::vector<server::RouteArg> args,
        server::QueryArgsList,
        auth::AuthManager & auth_manager,
        const publish::Publisher * publisher)
    {
        auto target = co_await beginPublication(request, args, auth_manager, publisher);
        if(!target) co_return std::move(target.error());

        auto claim = claimOf(*target);
        if(!claim) co_return std::move(claim.error());

        auto raw_tx = stringField(target->body, "raw_tx");
        if(!raw_tx) co_return std::move(raw_tx.error());

        const auto sent = co_await publisher->send(std::move(*claim), std::move(*raw_tx));
        if(!sent) co_return publishFailure(sent.error());

        http::Response response = makeResponse();
        response.setCode(http::Code::Accepted).setBodyWithContentLength(json{
            {"status", "pending"},
            {"tx_hash", *sent}
        }.dump());
        co_return response;
    }

    asio::awaitable<http::Response> POST_publishConfirm(
        const http::Request & request,
        std::vector<server::RouteArg> args,
        server::QueryArgsList,
        auth::AuthManager & auth_manager,
        const publish::Publisher * publisher)
    {
        auto target = co_await beginPublication(request, args, auth_manager, publisher);
        if(!target) co_return std::move(target.error());

        auto claim = claimOf(*target);
        if(!claim) co_return std::move(claim.error());

        auto tx_hash = stringField(target->body, "tx_hash");
        if(!tx_hash) co_return std::move(tx_hash.error());

        const auto confirmed = co_await publisher->confirm(std::move(*claim), std::move(*tx_hash));
        if(!confirmed) co_return publishFailure(confirmed.error());

        http::Response response = makeResponse();
        response.setCode(http::Code::Created).setBodyWithContentLength(json{
            {"status", "mined"},
            {"kind", std::string(pt::entityKindName(target->kind))},
            {"name", target->name},
            {"tx_hash", confirmed->tx_hash},
            {"block_number", confirmed->block_number},
            {"address", confirmed->entity_address},
            {"owner", confirmed->owner},
            {"content_hash", confirmed->content_hash}
        }.dump());
        co_return response;
    }
}
