#include "utils.hpp"

#include <algorithm>
#include <cctype>

namespace dcn::utils
{
    std::string loadBuildTimestamp(const std::filesystem::path & path) 
    {
        std::ifstream file(path);
        if (!file.is_open()) return "Unknown";
        std::string timestamp;
        std::getline(file, timestamp);
        return timestamp;
    }

    std::string currentTimestamp()
    {
        const auto now = std::chrono::system_clock::now();
        const auto now_time_t = std::chrono::system_clock::to_time_t(now);

        std::tm local_tm{};
        static std::mutex localtime_mutex;
        {
            std::lock_guard<std::mutex> lock(localtime_mutex);
            const auto* local_tm_ptr = std::localtime(&now_time_t);
            if (local_tm_ptr == nullptr)
            {
                throw std::runtime_error("Failed to convert current time");
            }
            local_tm = *local_tm_ptr;
        }

        return std::format("{:04d}-{:02d}-{:02d}-{:02d}_{:02d}_{:02d}",
            local_tm.tm_year + 1900,
            local_tm.tm_mon + 1,
            local_tm.tm_mday,
            local_tm.tm_hour,
            local_tm.tm_min,
            local_tm.tm_sec);
    }

    std::int64_t nowMs()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }

    std::string utcTimestamp(const std::chrono::system_clock::time_point time)
    {
        const auto seconds = std::chrono::floor<std::chrono::seconds>(time);
        const auto days = std::chrono::floor<std::chrono::days>(seconds);
        const std::chrono::year_month_day date{days};
        const std::chrono::hh_mm_ss clock{seconds - days};
        return std::format("{:04d}-{:02d}-{:02d}T{:02d}:{:02d}:{:02d}Z",
            static_cast<int>(date.year()), static_cast<unsigned>(date.month()), static_cast<unsigned>(date.day()),
            clock.hours().count(), clock.minutes().count(), clock.seconds().count());
    }

    bool equalsIgnoreCase(const std::string_view a, const std::string_view b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char a, char b) {
               return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
           });
    }

    std::string_view trimView(std::string_view value)
    {
        const auto is_space = [](const unsigned char ch) { return std::isspace(ch) != 0; };
        while(!value.empty() && is_space(value.front())) value.remove_prefix(1);
        while(!value.empty() && is_space(value.back())) value.remove_suffix(1);
        return value;
    }

    std::optional<std::string> trimAsciiWhitespace(const std::string_view value)
    {
        const std::string_view trimmed = trimView(value);
        if(trimmed.empty())
        {
            return std::nullopt;
        }
        return std::string(trimmed);
    }

    std::string toLower(std::string value)
    {
        std::ranges::transform(value, value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return value;
    }

    bool likelyNetworkPath(const std::filesystem::path & path)
    {
        const std::string raw = path.lexically_normal().string();
        return raw.rfind("\\\\", 0) == 0 || raw.rfind("//", 0) == 0;
    }


    void logException(const std::exception_ptr & exception_ptr, const std::string_view context)
    {
        if(!exception_ptr)
        {
            return;
        }

        try
        {
            std::rethrow_exception(exception_ptr);
        }
        catch(const std::exception & e)
        {
            spdlog::error("{}: {}", context, e.what());
        }
        catch(...)
        {
            spdlog::error("{}: unknown exception", context);
        }
    }

}
