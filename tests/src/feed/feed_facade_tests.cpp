#include <gtest/gtest.h>
#include "unit-tests.hpp"
#include "events_test_harness.hpp"
#include "feed.hpp"
#include "event_projector.hpp"

using namespace dcn;
using namespace dcn::tests;
using namespace dcn::tests::events_harness;

TEST_F(UnitTest, Feed_Facade_ApplyChangeThenQueryViaStrand)
{
    const auto paths = makeTempEventsPaths("feed_facade");
    asio::io_context io;
    dcn::feed::Feed feed(io, paths.feed_db, paths.feed_archive_root, 7LL*24*60*60*1000);

    dcn::events::ChangeRecord row;
    row.chain_id = CHAIN_ID;
    row.block_hash = hexBytes(0xB0, 32);
    row.change_seq = 1;
    row.event_type = std::string(dcn::events::CONNECTOR_ADDED_TYPE);
    row.state = std::string(dcn::events::FINALIZED_STATE);
    row.name = "alpha";
    row.owner = hexBytes(0xD0, 20);
    row.data_hex = "0x";
    row.block_number = 60;

    runAwaitable(io, feed.applyChange(row, 1'700'000'502'000));
    runAwaitable(io, feed.setFeedCursor(1));

    const auto page = feed.getFeedPage(dcn::feed::FeedQuery{.limit = 10});
    ASSERT_EQ(page.items.size(), 1u);
    EXPECT_EQ(feed.getFeedCursor(), 1);
}
