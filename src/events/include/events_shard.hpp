#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace dcn::events
{
    struct EventShardId
    {
        std::uint64_t chain_id = 1;
        int year = 1970;
        int month = 1;
    };

    class IEventShardRouter
    {
        public:
            virtual ~IEventShardRouter() = default;

            virtual EventShardId shardForFinalizedBlockTime(std::uint64_t chain_id, std::int64_t unix_seconds) const = 0;
            virtual std::filesystem::path filenameFor(const EventShardId & shard_id) const = 0;
            virtual std::string monthToken(const EventShardId & shard_id) const = 0;
    };

    class MonthlyEventShardRouter final : public IEventShardRouter
    {
        public:
            explicit MonthlyEventShardRouter(std::filesystem::path archive_root);

            EventShardId shardForFinalizedBlockTime(std::uint64_t chain_id, std::int64_t unix_seconds) const override;
            std::filesystem::path filenameFor(const EventShardId & shard_id) const override;
            std::string monthToken(const EventShardId & shard_id) const override;

        private:
            std::filesystem::path _archive_root;
    };
}
