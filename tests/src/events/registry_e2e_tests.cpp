#include "unit-tests.hpp"
#include "test_connector_helpers.hpp"
#include "events_test_harness.hpp"

#include "registry_projector.hpp"
#include "feed.hpp"
#include "feed_projector.hpp"

#include <array>
#include <chrono>
#include <filesystem>
#include <string>
#include <system_error>

#ifndef DECENTRALISED_ART_TEST_SOLC_PATH
    #error "DECENTRALISED_ART_TEST_SOLC_PATH is not defined"
#endif

#ifndef DECENTRALISED_ART_TEST_PT_PATH
    #error "DECENTRALISED_ART_TEST_PT_PATH is not defined"
#endif

using namespace dcn;
using namespace dcn::tests;
using namespace dcn::tests::events_harness;

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

    std::filesystem::path makeStoragePath(const std::string & test_name)
    {
        const auto suffix = std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count());
        return buildPath() / "tests" / "registry_e2e" / (test_name + "_" + suffix);
    }

    struct PathScope
    {
        explicit PathScope(std::filesystem::path path_) : path(std::move(path_)) {}
        ~PathScope()
        {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
        std::filesystem::path path;
    };

    bool prepareStorageLayout(const std::filesystem::path & storage_path)
    {
        static const std::array<std::string, 3> entity_dirs{
            "connectors", "transformations", "conditions"
        };
        std::error_code ec;
        for(const auto & dir : entity_dirs)
        {
            ec.clear();
            std::filesystem::create_directories(storage_path / dir / "build", ec);
            if(ec) return false;
        }
        return true;
    }
}

// ---------------------------------------------------------------------------
// End-to-end test: deploy a transformation + connector via the loader, then
// verify that the event pipeline (LocalEvmSource → EventRuntime →
// FeedProjector + RegistryProjector) materialises both entities into the
// Registry AND produces a feed item for the connector.
//
// This proves that one EVM event stream drives BOTH the feed store and the
// registry simultaneously.
// ---------------------------------------------------------------------------
TEST_F(UnitTest, Events_Registry_E2E_ConnectorAndTransformationMaterializeInBothStores)
{
    ASSERT_TRUE(std::filesystem::exists(solcPath()))
        << std::format("Missing Solidity compiler at '{}'", solcPath().string());
    ASSERT_TRUE(std::filesystem::exists(ptPath() / "contracts"))
        << std::format("Missing PT contracts at '{}'", (ptPath() / "contracts").string());

    const auto storage_path = makeStoragePath("e2e_deploy");
    PathScope storage_scope(storage_path);
    ASSERT_TRUE(prepareStorageLayout(storage_path));

    const auto events_paths = makeTempEventsPaths("registry_e2e");

    asio::io_context io;
    registry::Registry registry(io);      // in-memory registry
    evm::EVM evm(io, EVMC_SHANGHAI, solcPath(), ptPath());
    io.run();  // initialise EVM (stops io_context)

    const chain::Address caller = helpers::makeAddressFromByte(0xE1);
    helpers::runAwaitable(io, evm.addAccount(caller, evm::DEFAULT_GAS_LIMIT));
    helpers::runAwaitable(io, evm.setGas(caller, evm::DEFAULT_GAS_LIMIT));

    const std::string owner_hex = chain::addressToHex(caller);
    const std::string tf_name   = "e2e_scale_tf";
    const std::string conn_name = "e2e_connector";

    // Build EventRuntime with LocalEvmSource; both projectors wired explicitly (symmetric).
    events::EventRuntime event_runtime(
        io,
        events::EventRuntimeConfig{
            .hot_db_path         = events_paths.hot_db,
            .chain_id            = 1,
            .ingestion_enabled   = true,
            .sources             = {std::make_shared<events::LocalEvmSource>(evm, 1)},
            .poll_interval_ms    = 50,   // fast poll for tests
            .projector_interval_ms = 50  // fast projection for tests
        });

    // Create feed DB alongside the events DB.
    const auto feed_db_path = events_paths.root / "feed.sqlite";
    const auto feed_archive_path = events_paths.root / "feed_archive";
    {
        std::error_code feed_ec;
        std::filesystem::create_directories(feed_archive_path, feed_ec);
        ASSERT_FALSE(feed_ec) << "Failed to create feed archive dir: " << feed_ec.message();
    }

    dcn::feed::Feed feed(
        io,
        feed_db_path,
        feed_archive_path,
        /*outbox_retention_ms=*/ 7LL * 24 * 60 * 60 * 1000);

    // FeedProjector (feed module) first — drives feed DB from events hot-store changes.
    event_runtime.addProjector(std::make_unique<dcn::feed::FeedProjector>(
        event_runtime.projectionStore(), feed, event_runtime.writeStrand()));
    event_runtime.addProjector(std::make_unique<registry::RegistryProjector>(
        event_runtime.projectionStore(),
        registry,
        event_runtime.writeStrand()));

    event_runtime.start();

    bool tf_in_registry   = false;
    bool conn_in_registry = false;

    auto test_coro = [&]() -> asio::awaitable<void>
    {
        // -- 1. Deploy transformation -------------------------------------------
        TransformationRecord tf_record;
        tf_record.set_owner(owner_hex);
        tf_record.mutable_transformation()->set_name(tf_name);
        tf_record.mutable_transformation()->set_sol_src("return x * 2;");
        tf_record.mutable_transformation()->set_args_count(1);

        const auto tf_result = co_await loader::deployTransformation(
            evm, registry, tf_record, storage_path, /*persist_json=*/false);

        if(!tf_result.has_value())
        {
            ADD_FAILURE() << "Transformation deploy failed (kind="
                          << static_cast<int>(tf_result.error().kind) << ")";
            co_await event_runtime.stop();
            co_return;
        }

        // -- 2. Deploy connector with transformation dependency -----------------
        ConnectorRecord conn_record;
        conn_record.set_owner(owner_hex);
        conn_record.mutable_connector()->set_name(conn_name);
        auto * dim = conn_record.mutable_connector()->add_dimensions();
        auto * tf_def = dim->add_transformations();
        tf_def->set_name(tf_name);

        const auto conn_result = co_await loader::deployConnector(
            evm, registry, conn_record, storage_path, /*persist_json=*/false);

        if(!conn_result.has_value())
        {
            ADD_FAILURE() << "Connector deploy failed (kind="
                          << static_cast<int>(conn_result.error().kind) << ")";
            co_await event_runtime.stop();
            co_return;
        }

        // -- 3. Poll registry until both entities appear (or timeout) ----------
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        asio::steady_timer timer(co_await asio::this_coro::executor);

        while(std::chrono::steady_clock::now() < deadline)
        {
            tf_in_registry   = co_await registry.hasTransformation(tf_name);
            conn_in_registry = co_await registry.hasConnector(conn_name);
            if(tf_in_registry && conn_in_registry) break;

            timer.expires_after(std::chrono::milliseconds(25));
            std::error_code ec;
            co_await timer.async_wait(asio::redirect_error(asio::use_awaitable, ec));
            if(ec) break;
        }

        co_await event_runtime.stop();
    };

    // Use use_future so exceptions from the coroutine propagate to the test.
    auto future = asio::co_spawn(io, test_coro(), asio::use_future);
    io.restart();
    io.run();
    future.get();  // propagate any exception from the coroutine

    // ---- Assert: both entities in Registry ------------------------------------
    EXPECT_TRUE(tf_in_registry)
        << "Transformation '" << tf_name << "' was not materialised in the registry within 10 s";
    EXPECT_TRUE(conn_in_registry)
        << "Connector '" << conn_name << "' was not materialised in the registry within 10 s";

    // ---- Assert: feed item present for the connector --------------------------
    // feed::FeedProjector runs BEFORE RegistryProjector in every projector loop iteration.
    // By the time conn_in_registry is true, the feed DB already has the connector row.
    const dcn::feed::FeedPage feed_page = feed.getFeedPage(dcn::feed::FeedQuery{
        .limit               = 20,
        .include_unfinalized = false
    });

    EXPECT_GE(feed_page.items.size(), 1u)
        << "Expected at least one finalised feed item after end-to-end pipeline; got "
        << feed_page.items.size();

    bool found_connector_feed = false;
    for(const auto & item : feed_page.items)
    {
        if(item.event_type == "connector_added" &&
           item.payload.contains("name") &&
           item.payload.at("name").get<std::string>() == conn_name)
        {
            found_connector_feed = true;
            break;
        }
    }
    EXPECT_TRUE(found_connector_feed)
        << "No CONNECTOR_ADDED feed item with name='" << conn_name << "' found in the feed page";
}
