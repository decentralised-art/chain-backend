#include <gtest/gtest.h>

#include "unit-tests.hpp"
#include "events_test_harness.hpp"
#include "sqlite_feed_store.hpp"
#include "event_projector.hpp"

using namespace dcn;
using namespace dcn::tests;
using namespace dcn::tests::events_harness;

TEST_F(UnitTest, Feed_SqliteStore_ApplyChange_ProducesFeedItem)
{
    const auto paths = makeTempEventsPaths("feed_store");
    dcn::feed::SqliteFeedStore store(paths.feed_db, paths.feed_archive_root, 7LL*24*60*60*1000);

    dcn::events::ChangeRecord row;
    row.chain_id = CHAIN_ID;
    row.block_hash = hexBytes(0xB0, 32);
    row.log_index = 0;
    row.change_seq = 1;
    row.event_type = std::string(dcn::events::CONNECTOR_ADDED_TYPE);
    row.state = std::string(dcn::events::FINALIZED_STATE);
    row.name = "alpha";
    row.owner = hexBytes(0xD0, 20);
    row.data_hex = "0x";
    row.block_number = 60;
    row.tx_hash = hexBytes(0xAA, 32);
    row.tx_index = 0;

    store.applyChange(row, 1'700'000'502'000);

    const dcn::feed::FeedPage page = store.getFeedPage(dcn::feed::FeedQuery{.limit = 10});
    ASSERT_EQ(page.items.size(), 1u);
    EXPECT_EQ(page.items[0].event_type, std::string(dcn::events::CONNECTOR_ADDED_TYPE));
    EXPECT_GT(store.getFeedCursor(), -1);
}
