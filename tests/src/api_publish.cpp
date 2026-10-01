#include "unit-tests.hpp"

#include "fake_eth_node.hpp"

#include <array>
#include <filesystem>
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
using namespace dcn::tests::events_harness;
using namespace dcn::tests::fake_eth;

namespace
{
    std::filesystem::path solcPath()
    {
        return std::filesystem::path(DECENTRALISED_ART_TEST_SOLC_PATH);
    }

    std::filesystem::path ptPath()
    {
        return std::filesystem::path(DECENTRALISED_ART_TEST_PT_PATH);
    }

    server::RouteArg kindArg(const std::string & kind)
    {
        return server::RouteArg(
            server::RouteArgDef(server::RouteArgType::string, server::RouteArgRequirement::required),
            kind);
    }

    http::Request postRequest(const std::string & path, const std::string & token, const json & body)
    {
        http::Request request;
        request.setMethod(http::Method::POST)
               .setPath(http::URL(path))
               .setVersion("HTTP/1.1")
               .setBody(body.dump());
        if(!token.empty())
        {
            request.setHeader(http::Header::Authorization, std::format("Bearer {}", token));
        }
        return request;
    }

    json bodyOf(const http::Response & response)
    {
        return json::parse(response.getBody(), nullptr, false);
    }

    std::filesystem::path makeStorage(const std::filesystem::path & root)
    {
        const auto storage = root / "storage";
        for(const char * kind : {"connectors", "transformations", "conditions"})
        {
            std::filesystem::create_directories(storage / kind / "build");
        }
        return storage;
    }

    registry::EntitySource chainSource()
    {
        registry::EntitySource source;
        source.source_id = std::format("eip155:{}:{}", CHAIN_ID, REGISTRY_ADDRESS);
        source.chain_namespace = std::format("eip155:{}", CHAIN_ID);
        source.chain_id = CHAIN_ID;
        source.registry_address = REGISTRY_ADDRESS;
        return source;
    }

    http::Request makeExecuteRequest(const std::string & connector_name)
    {
        return postRequest("/execute", "", json{
            {"connector_name", connector_name},
            {"particles_count", 3},
            {"dynamic_ri", json::object()}});
    }
}

// ---------------------------------------------------------------------------
// Local entity names
// ---------------------------------------------------------------------------

TEST_F(UnitTest, API_AdmitLocalEntityName_RefusesInvalidAndChainOwnedNames)
{
    asio::io_context io_context;
    registry::Registry registry(io_context);
    auto node = std::make_shared<FakeNode>();
    const ChainAccess chain_access{.client = makeClient(node), .registry = REGISTRY_ADDRESS, .runner = RUNNER_ADDRESS};

    const auto invalid = runAwaitable(
        io_context, admitLocalEntityName(registry, &chain_access, pt::EntityKind::TRANSFORMATION, "has space"));
    ASSERT_TRUE(invalid.has_value());
    EXPECT_EQ(invalid->getCode(), http::Code::BadRequest);

    // Without a chain only the projected registry is asked.
    EXPECT_FALSE(runAwaitable(
        io_context, admitLocalEntityName(registry, nullptr, pt::EntityKind::TRANSFORMATION, "Free")).has_value());
    EXPECT_FALSE(runAwaitable(
        io_context, admitLocalEntityName(registry, &chain_access, pt::EntityKind::TRANSFORMATION, "Free")).has_value());

    // The live registry already holds it, although the projector has not caught up.
    node->contains_entity = true;
    const auto live = runAwaitable(
        io_context, admitLocalEntityName(registry, &chain_access, pt::EntityKind::TRANSFORMATION, "Taken"));
    ASSERT_TRUE(live.has_value());
    EXPECT_EQ(live->getCode(), http::Code::Conflict);

    // A live check that cannot complete refuses the name rather than risk a collision.
    node->contains_entity = false;
    node->contains_error = rpc::RpcError{.kind = rpc::RpcError::Kind::TRANSPORT, .code = -32603, .message = "internal error"};
    const auto unavailable = runAwaitable(
        io_context, admitLocalEntityName(registry, &chain_access, pt::EntityKind::TRANSFORMATION, "Unknown"));
    ASSERT_TRUE(unavailable.has_value());
    EXPECT_EQ(unavailable->getCode(), http::Code::ServiceUnavailable);
}

TEST_F(UnitTest, API_AdmitLocalEntityName_RefusesANameProjectedFromAChain)
{
    asio::io_context io_context;
    registry::Registry registry(io_context);

    TransformationRecord record;
    record.set_owner(chain::addressToHex(makeAddressFromByte(0x42)));
    record.mutable_transformation()->set_name("OnChain");
    ASSERT_TRUE(runAwaitable(io_context, registry.addTransformationVersion(chainSource(), makeAddressFromByte(0x61), record)));

    const auto refused = runAwaitable(
        io_context, admitLocalEntityName(registry, nullptr, pt::EntityKind::TRANSFORMATION, "OnChain"));
    ASSERT_TRUE(refused.has_value());
    EXPECT_EQ(refused->getCode(), http::Code::Conflict);

    // Names are per kind: a condition may use it.
    EXPECT_FALSE(runAwaitable(
        io_context, admitLocalEntityName(registry, nullptr, pt::EntityKind::CONDITION, "OnChain")).has_value());
}

// ---------------------------------------------------------------------------
// Publication routes
// ---------------------------------------------------------------------------

TEST_F(UnitTest, API_PublishConfirm_ValidatesRouteAuthenticationAndBody)
{
    asio::io_context io_context;
    auth::AuthManager auth_manager(io_context);
    auto node = std::make_shared<FakeNode>();
    const auto paths = makeTempEventsPaths("api_publish_confirm_validation");
    const pt::ArtifactStore artifacts(paths.root / "cache", {}, {paths.root / "replica"});
    const publish::Publisher publisher(makeClient(node), makeManifest(), artifacts);

    const chain::Address owner = makeAddressFromByte(0x42);
    const std::string token = runAwaitable(io_context, auth_manager.generateAccessToken(owner));
    const json body{{"name", "PubTx"}, {"content_hash", hexBytes(0xAA, 32)}, {"tx_hash", hexBytes(0x99, 32)}};

    const auto confirm = [&](const std::string & kind, const std::string & request_token, const json & request_body,
                             const publish::Publisher * target)
    {
        return runAwaitable(io_context, POST_publishConfirm(
            postRequest("/publish/" + kind, request_token, request_body), {kindArg(kind)}, {}, auth_manager, target));
    };

    EXPECT_EQ(confirm("transformation", token, body, nullptr).getCode(), http::Code::Forbidden);
    EXPECT_EQ(confirm("widget", token, body, &publisher).getCode(), http::Code::NotFound);
    EXPECT_EQ(confirm("transformation", "", body, &publisher).getCode(), http::Code::Unauthorized);

    json bad_name = body;
    bad_name["name"] = "../escape";
    EXPECT_EQ(confirm("transformation", token, bad_name, &publisher).getCode(), http::Code::BadRequest);

    json bad_hash = body;
    bad_hash["content_hash"] = "0x1234";
    EXPECT_EQ(confirm("transformation", token, bad_hash, &publisher).getCode(), http::Code::BadRequest);

    json no_tx = body;
    no_tx.erase("tx_hash");
    EXPECT_EQ(confirm("transformation", token, no_tx, &publisher).getCode(), http::Code::BadRequest);
}

TEST_F(UnitTest, API_PublishConfirm_AnswersPendingUntilTheRegistryEventIsMined)
{
    asio::io_context io_context;
    auth::AuthManager auth_manager(io_context);
    auto node = std::make_shared<FakeNode>();
    const auto paths = makeTempEventsPaths("api_publish_confirm");
    const pt::ArtifactStore artifacts(paths.root / "cache", {}, {paths.root / "replica"});
    const publish::Publisher publisher(makeClient(node), makeManifest(), artifacts);

    const chain::Address owner = makeAddressFromByte(0x42);
    const std::string token = runAwaitable(io_context, auth_manager.generateAccessToken(owner));
    const std::string tx_hash = hexBytes(0x99, 32);
    const evmc::bytes32 content_hash = bytes32OfFill(0xAA);
    const json body{{"name", "PubTx"}, {"content_hash", chain::bytes32ToHex(content_hash)}, {"tx_hash", tx_hash}};

    const auto confirm = [&]()
    {
        return runAwaitable(io_context, POST_publishConfirm(
            postRequest("/publish/transformation", token, body), {kindArg("transformation")}, {}, auth_manager, &publisher));
    };

    const auto pending = confirm();
    ASSERT_EQ(pending.getCode(), http::Code::Accepted) << pending.getBody();
    EXPECT_EQ(bodyOf(pending)["status"], "pending");
    EXPECT_EQ(bodyOf(pending)["tx_hash"], tx_hash);

    node->receipt = makeTransformationAddedReceipt(
        tx_hash, "PubTx", owner, content_hash, pt::computeMetadataHash(pt::EntityKind::TRANSFORMATION, "PubTx"));
    const auto mined = confirm();
    ASSERT_EQ(mined.getCode(), http::Code::Created) << mined.getBody();
    const json result = bodyOf(mined);
    EXPECT_EQ(result["status"], "mined");
    EXPECT_EQ(result["kind"], "transformation");
    EXPECT_EQ(result["block_number"], 99);
    EXPECT_EQ(result["owner"], chain::addressToHex(owner));
    EXPECT_EQ(result["address"], hexAddress(0x55));
}

TEST_F(UnitTest, API_PublishSend_ValidatesRouteAuthenticationAndBody)
{
    asio::io_context io_context;
    auth::AuthManager auth_manager(io_context);
    auto node = std::make_shared<FakeNode>();
    const auto paths = makeTempEventsPaths("api_publish_send_validation");
    const pt::ArtifactStore artifacts(paths.root / "cache", {}, {paths.root / "replica"});
    const publish::Publisher publisher(makeClient(node), makeManifest(), artifacts);

    const auto owner = makeOwnerSigner();
    const pt::Publication publication = makeRelayedPublication(owner->address(), "PubTx");
    const std::string token = runAwaitable(io_context, auth_manager.generateAccessToken(owner->address()));
    const json body{
        {"name", "PubTx"},
        {"content_hash", chain::bytes32ToHex(publication.content_hash)},
        {"raw_tx", signTransaction(*owner, makePublicationTransaction(publication))}};

    const auto send = [&](const std::string & kind, const std::string & request_token, const json & request_body,
                          const publish::Publisher * target)
    {
        return runAwaitable(io_context, POST_publishSend(
            postRequest("/publish/" + kind + "/send", request_token, request_body),
            {kindArg(kind)}, {}, auth_manager, target));
    };

    EXPECT_EQ(send("transformation", token, body, nullptr).getCode(), http::Code::Forbidden);
    EXPECT_EQ(send("widget", token, body, &publisher).getCode(), http::Code::NotFound);
    EXPECT_EQ(send("transformation", "", body, &publisher).getCode(), http::Code::Unauthorized);

    json bad_hash = body;
    bad_hash["content_hash"] = "0x1234";
    EXPECT_EQ(send("transformation", token, bad_hash, &publisher).getCode(), http::Code::BadRequest);

    json no_raw = body;
    no_raw.erase("raw_tx");
    EXPECT_EQ(send("transformation", token, no_raw, &publisher).getCode(), http::Code::BadRequest);

    json garbage = body;
    garbage["raw_tx"] = "0x02c0";
    EXPECT_EQ(send("transformation", token, garbage, &publisher).getCode(), http::Code::BadRequest);

    EXPECT_TRUE(node->sent_raw_transactions.empty());
}

TEST_F(UnitTest, API_PublishSend_ForwardsOnlyTheOwnersSignedPublication)
{
    asio::io_context io_context;
    auth::AuthManager auth_manager(io_context);
    auto node = std::make_shared<FakeNode>();
    const auto paths = makeTempEventsPaths("api_publish_send");
    const pt::ArtifactStore artifacts(paths.root / "cache", {}, {paths.root / "replica"});
    const publish::Publisher publisher(makeClient(node), makeManifest(), artifacts);

    const auto owner = makeOwnerSigner();
    const pt::Publication publication = makeRelayedPublication(owner->address(), "PubTx");
    const std::string raw = signTransaction(*owner, makePublicationTransaction(publication));
    const json body{
        {"name", "PubTx"},
        {"content_hash", chain::bytes32ToHex(publication.content_hash)},
        {"raw_tx", raw}};

    const auto send = [&](const chain::Address & caller)
    {
        const std::string token = runAwaitable(io_context, auth_manager.generateAccessToken(caller));
        return runAwaitable(io_context, POST_publishSend(
            postRequest("/publish/transformation/send", token, body),
            {kindArg("transformation")}, {}, auth_manager, &publisher));
    };

    // Another account's session cannot pass the owner's transaction off as its own.
    EXPECT_EQ(send(makeAddressFromByte(0x43)).getCode(), http::Code::Forbidden);
    EXPECT_TRUE(node->sent_raw_transactions.empty());

    const auto sent = send(owner->address());
    ASSERT_EQ(sent.getCode(), http::Code::Accepted) << sent.getBody();
    EXPECT_EQ(bodyOf(sent)["status"], "pending");
    EXPECT_EQ(bodyOf(sent)["tx_hash"], publish::transactionHash(raw));
    ASSERT_EQ(node->sent_raw_transactions.size(), 1u);
    EXPECT_EQ(node->sent_raw_transactions.front(), raw);

    // A used account nonce is resolved by preparing again.
    node->send_error = rpc::RpcError{.kind = rpc::RpcError::Kind::EXECUTION, .code = -32000, .message = "nonce too low"};
    EXPECT_EQ(send(owner->address()).getCode(), http::Code::Conflict);
}

TEST_F(UnitTest, API_PublishPrepare_RebuildsTheCallersLocalEntityIntoAnOwnerPaidTransaction)
{
    ASSERT_TRUE(std::filesystem::exists(solcPath())) << std::format("Missing Solidity compiler at '{}'", solcPath().string());
    ASSERT_TRUE(std::filesystem::exists(ptPath() / "contracts")) << std::format("Missing PT contracts at '{}'", (ptPath() / "contracts").string());

    const auto paths = makeTempEventsPaths("api_publish_prepare");
    const auto storage = makeStorage(paths.root);

    asio::io_context io_context;
    registry::Registry registry(io_context);
    auth::AuthManager auth_manager(io_context);
    evm::EVM evm_instance(io_context, EVMC_SHANGHAI, solcPath(), ptPath());
    io_context.run();

    const chain::Address owner = makeAddressFromByte(0x42);
    TransformationRecord record;
    record.set_owner(chain::addressToHex(owner));
    record.mutable_transformation()->set_name("PubTx");
    record.mutable_transformation()->set_sol_src("return x;");
    ASSERT_TRUE(runAwaitable(io_context, loader::deployTransformation(evm_instance, registry, record, storage)).has_value());

    auto node = std::make_shared<FakeNode>();
    const pt::ArtifactStore artifacts(storage / "artifacts" / "cache", {}, {paths.root / "replica"});
    const publish::Publisher publisher(makeClient(node), makeManifest(), artifacts);
    config::Config cfg;
    cfg.storage_path = storage;

    const auto prepareWith = [&](const chain::Address & caller, const json & request_body)
    {
        const std::string token = runAwaitable(io_context, auth_manager.generateAccessToken(caller));
        return runAwaitable(io_context, POST_publishPrepare(
            postRequest("/publish/transformation/prepare", token, request_body),
            {kindArg("transformation")}, {}, auth_manager, evm_instance, &publisher, cfg));
    };
    const auto prepare = [&](const chain::Address & caller, const std::string & name)
    {
        return prepareWith(caller, json{{"name", name}});
    };

    const auto prepared = prepare(owner, "PubTx");
    ASSERT_EQ(prepared.getCode(), http::Code::OK) << prepared.getBody();
    const json body = bodyOf(prepared);
    EXPECT_EQ(body["status"], "prepared");
    EXPECT_EQ(body["transaction"]["from"], chain::addressToHex(owner));
    EXPECT_EQ(body["transaction"]["to"], REGISTRY_ADDRESS);
    EXPECT_EQ(body["transaction"]["chainId"], "0x1");
    EXPECT_EQ(body["transaction"]["gas"], "0x2bf20");
    EXPECT_TRUE(body["transaction"]["data"].get<std::string>().starts_with("0x"));
    EXPECT_EQ(body["publication_nonce"], 3);
    // A browser wallet picks its own nonce and fees.
    EXPECT_FALSE(body.contains("signing"));

    // A relay adds what an offline signer needs to complete the transaction.
    const auto relayed = prepareWith(owner, json{{"name", "PubTx"}, {"relay", true}});
    ASSERT_EQ(relayed.getCode(), http::Code::OK) << relayed.getBody();
    const json signing = bodyOf(relayed)["signing"];
    EXPECT_EQ(signing["type"], "0x2");
    EXPECT_EQ(signing["nonce"], "0x7");
    EXPECT_EQ(signing["maxFeePerGas"], "0xd09dc300");          // 2 x 1 gwei base fee + 1.5 gwei tip
    EXPECT_EQ(signing["maxPriorityFeePerGas"], "0x59682f00");  // 1.5 gwei
    EXPECT_EQ(signing["value"], "0x0");
    EXPECT_EQ(prepareWith(owner, json{{"name", "PubTx"}, {"relay", "yes"}}).getCode(), http::Code::BadRequest);

    // The artifact is durable before the owner can send, and the server sent nothing.
    const std::string content_hash = body["content_hash"];
    EXPECT_EQ(pt::ArtifactStore(paths.root / "replica").read(content_hash).state, pt::ArtifactState::AVAILABLE);
    EXPECT_TRUE(node->sent_raw_transactions.empty());

    // Only the owner can publish it, and only what exists locally can be rebuilt.
    EXPECT_EQ(prepare(makeAddressFromByte(0x43), "PubTx").getCode(), http::Code::Forbidden);
    EXPECT_EQ(prepare(owner, "NeverCreated").getCode(), http::Code::NotFound);

    // The registry already holds exactly this publication: there is nothing to send.
    node->contains_entity = true;
    node->entity_content_hash = content_hash;
    const auto published = prepare(owner, "PubTx");
    ASSERT_EQ(published.getCode(), http::Code::OK) << published.getBody();
    EXPECT_EQ(bodyOf(published)["status"], "published");

    // Something else under the name is a conflict.
    node->entity_content_hash = wordOfFill(0x11);
    EXPECT_EQ(prepare(owner, "PubTx").getCode(), http::Code::Conflict);
}

// ---------------------------------------------------------------------------
// Execution
// ---------------------------------------------------------------------------

TEST_F(UnitTest, API_Execute_RunsTheConnectorAsAGasCappedEthCallOnTheRunner)
{
    asio::io_context io_context;

    auto node = std::make_shared<FakeNode>();
    std::string runner_answer = "particles";
    node->call_override = [&runner_answer](const json & request) -> std::string
    {
        if(chain::normalizeHex(request.at("params").at(0).value("to", "")) != RUNNER_ADDRESS) return {};
        if(runner_answer == "particles")
        {
            return rpcResult(request, encodeParticlesResult({{"/ChainConnector:0", {7, 8, 9}}}));
        }

        std::vector<std::uint8_t> revert;
        if(runner_answer == "condition")
        {
            revert = chain::constructSelector("ConditionNotMet(bytes32)");
            revert.resize(revert.size() + 32, 0);
        }
        else
        {
            revert = chain::constructSelector("Error(string)");
            const auto reason = pt::abi::encodeTuple({pt::abi::stringComponent(runner_answer)});
            revert.insert(revert.end(), reason.begin(), reason.end());
        }
        return rpcError(request, 3, "execution reverted", chain::bytesToHex(revert));
    };
    const ChainAccess chain_access{
        .client = makeClient(node),
        .registry = REGISTRY_ADDRESS,
        .runner = RUNNER_ADDRESS,
        .execute_max_gas = 1'000'000,
        .execute_block_tag = "safe"};

    // No login: the request carries no Authorization header.
    const auto execute = [&](const ChainAccess * access)
    {
        return runAwaitable(io_context, POST_execute(makeExecuteRequest("ChainConnector"), {}, {}, access));
    };

    // The chain answers alone: this server's registry knows nothing about the connector.
    const auto response = execute(&chain_access);
    ASSERT_EQ(response.getCode(), http::Code::OK) << response.getBody();
    const json body = bodyOf(response);
    EXPECT_EQ(body["particles"], json::parse(R"([{"path":"/ChainConnector:0","data":[7,8,9]}])"));

    // The answer names the block it came from: the configured tag resolved to one block,
    // and the call pinned to that block's hash.
    EXPECT_EQ(node->requested_blocks.back(), "safe");
    EXPECT_EQ(body["block_number"], node->head_block);
    EXPECT_EQ(body["block_hash"], hexBytes(0x31, 32));
    EXPECT_EQ(body["runner"], RUNNER_ADDRESS);

    // One eth_call of Runner.gen at exactly that block, bounded by the configured gas.
    ASSERT_FALSE(node->calls.empty());
    const json & call = node->calls.back();
    EXPECT_EQ(call.at(1).at("blockHash"), hexBytes(0x31, 32));
    EXPECT_EQ(call.at(1).at("requireCanonical"), true);
    EXPECT_EQ(call.at(0).at("gas"), "0xf4240");
    EXPECT_TRUE(call.at(0).at("data").get<std::string>().starts_with(
        selectorOf("gen(string,uint32,(uint32,uint32,uint32)[])")));

    // A PT error is the connector refusing the request.
    runner_answer = "condition";
    const auto rejected = execute(&chain_access);
    EXPECT_EQ(rejected.getCode(), http::Code::BadRequest);
    EXPECT_NE(bodyOf(rejected)["message"].get<std::string>().find("Execution rejected"), std::string::npos);

    // The runner's registry does not hold the name.
    runner_answer = "cannot find connector";
    EXPECT_EQ(execute(&chain_access).getCode(), http::Code::NotFound);

    ChainAccess no_runner = chain_access;
    no_runner.runner.clear();
    EXPECT_EQ(execute(&no_runner).getCode(), http::Code::ServiceUnavailable);
    EXPECT_EQ(execute(nullptr).getCode(), http::Code::ServiceUnavailable);

    // Without the configured block there is no state to answer from.
    node->serve_block_tags = false;
    runner_answer = "particles";
    EXPECT_EQ(execute(&chain_access).getCode(), http::Code::ServiceUnavailable);
}

TEST_F(UnitTest, API_Simulate_ValidatesTheRequestAndRefusesUnavailableChainConnectors)
{
    asio::io_context io_context;
    registry::Registry registry(io_context);

    ASSERT_TRUE(std::filesystem::exists(solcPath())) << std::format("Missing Solidity compiler at '{}'", solcPath().string());
    evm::EVM evm_instance(io_context, EVMC_SHANGHAI, solcPath(), ptPath());
    io_context.run();
    config::Config cfg;

    // No login: the request carries no Authorization header.
    const auto simulate = [&](const json & body)
    {
        return runAwaitable(io_context, POST_simulate(
            postRequest("/simulate", "", body), {}, {}, registry, evm_instance, cfg));
    };

    EXPECT_EQ(simulate(json{{"connector_name", "Any"}, {"particles_count", 0}}).getCode(), http::Code::BadRequest);
    EXPECT_EQ(simulate(json{{"connector_name", "Any"}, {"particles_count", 65537}}).getCode(), http::Code::BadRequest);

    json too_many = json{{"connector_name", "Any"}, {"particles_count", 1}, {"dynamic_ri", json::object()}};
    for(int position = 0; position <= 4096; ++position)
    {
        too_many["dynamic_ri"][std::to_string(position)] = json{{"start_point", 0}, {"transformation_shift", 0}};
    }
    EXPECT_EQ(simulate(too_many).getCode(), http::Code::BadRequest);

    EXPECT_EQ(simulate(json{{"connector_name", "Missing"}, {"particles_count", 1}}).getCode(), http::Code::NotFound);

    // A connector that exists only on chain cannot be simulated until its artifacts are available.
    ConnectorRecord record;
    record.set_owner(chain::addressToHex(makeAddressFromByte(0x42)));
    record.mutable_connector()->set_name("ChainOnly");
    record.mutable_connector()->add_dimensions();
    ASSERT_TRUE(runAwaitable(io_context, registry.addConnectorVersion(chainSource(), makeAddressFromByte(0x60), record)));
    EXPECT_EQ(simulate(json{{"connector_name", "ChainOnly"}, {"particles_count", 1}}).getCode(), http::Code::ServiceUnavailable);
}
