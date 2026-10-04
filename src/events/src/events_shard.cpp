#include <chrono>
#include <format>

#include "events_shard.hpp"

namespace dcn::events
{
    MonthlyEventShardRouter::MonthlyEventShardRouter(std::filesystem::path archive_root)
        : _archive_root(std::move(archive_root))
    {
    }

    EventShardId MonthlyEventShardRouter::shardForFinalizedBlockTime(const std::uint64_t chain_id, const std::int64_t unix_seconds) const
    {
        const std::chrono::sys_seconds time{std::chrono::seconds{unix_seconds}};
        const std::chrono::year_month_day date{std::chrono::floor<std::chrono::days>(time)};
        return EventShardId{
            .chain_id = chain_id,
            .year = static_cast<int>(date.year()),
            .month = static_cast<int>(static_cast<unsigned>(date.month()))
        };
    }

    std::filesystem::path MonthlyEventShardRouter::filenameFor(const EventShardId & shard_id) const
    {
        return _archive_root
            / std::format("chain-{}", shard_id.chain_id)
            / std::format("{:04d}-{:02d}.sqlite", shard_id.year, shard_id.month);
    }

    std::string MonthlyEventShardRouter::monthToken(const EventShardId & shard_id) const
    {
        return std::format("{:04d}-{:02d}", shard_id.year, shard_id.month);
    }
}