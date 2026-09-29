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
        std::time_t tt = static_cast<std::time_t>(unix_seconds);
        std::tm tm_utc{};
#if defined(_WIN32)
        gmtime_s(&tm_utc, &tt);
#else
        gmtime_r(&tt, &tm_utc);
#endif
        return EventShardId{
            .chain_id = chain_id,
            .year = tm_utc.tm_year + 1900,
            .month = tm_utc.tm_mon + 1
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