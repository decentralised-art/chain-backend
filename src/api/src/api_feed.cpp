#include "api.hpp"

#include <algorithm>
#include <chrono>
#include <format>
#include <limits>
#include <utility>

namespace dcn
{
    namespace
    {
        constexpr std::size_t MAX_FEED_LIMIT = 256;
        constexpr std::size_t MAX_STREAM_LIMIT = 2048;
    }

    asio::awaitable<http::Response> OPTIONS_feed(const http::Request &, std::vector<server::RouteArg>, server::QueryArgsList)
    {
        http::Response response;
        response.setCode(http::Code::NoContent)
            .setVersion("HTTP/1.1")
            .setHeader(http::Header::AccessControlAllowOrigin, "*")
            .setHeader(http::Header::AccessControlAllowMethods, "GET, OPTIONS")
            .setHeader(http::Header::AccessControlAllowHeaders, "Content-Type")
            .setHeader(http::Header::AccessControlMaxAge, "600")
            .setHeader(http::Header::Connection, "close");
        co_return response;
    }

    asio::awaitable<http::Response> GET_feed(
        const http::Request &,
        std::vector<server::RouteArg> route_args,
        server::QueryArgsList query_args,
        feed::IFeedRepository & feed_repo)
    {
        http::Response response;
        response.setCode(http::Code::Unknown)
            .setVersion("HTTP/1.1")
            .setHeader(http::Header::AccessControlAllowOrigin, "*")
            .setHeader(http::Header::Connection, "close")
            .setHeader(http::Header::ContentType, "application/json");

        if(!route_args.empty())
        {
            response.setCode(http::Code::BadRequest)
                .setBodyWithContentLength(json{
                    {"message", "Invalid route arguments"}
                }.dump());
            co_return response;
        }

        if(!query_args.contains("limit"))
        {
            response.setCode(http::Code::BadRequest)
                .setBodyWithContentLength(json{
                    {"message", "Missing argument limit"}
                }.dump());
            co_return response;
        }

        auto limit_res = parse::parseRouteArgAs<std::size_t>(query_args.at("limit"));
        if(limit_res && (limit_res.value() == 0 || limit_res.value() > MAX_FEED_LIMIT))
        {
            limit_res = std::unexpected(parse::ParseError{parse::ParseError::Kind::OUT_OF_RANGE});
        }
        if(!limit_res)
        {
            response.setCode(http::Code::BadRequest)
                .setBodyWithContentLength(json{
                    {"message", std::format("Invalid argument limit. limit error: {}.", limit_res.error().kind)}
                }.dump());
            co_return response;
        }

        feed::FeedQuery feed_query;
        feed_query.limit = *limit_res;

        if(query_args.contains("before"))
        {
            auto before_res = parse::parseRouteArgAs<std::string>(query_args.at("before"));
            if(!before_res)
            {
                response.setCode(http::Code::BadRequest)
                    .setBodyWithContentLength(json{
                        {"message", "Invalid before cursor"}
                    }.dump());
                co_return response;
            }
            feed_query.before_cursor = *before_res;
        }

        if(query_args.contains("type"))
        {
            auto type_res = parse::parseRouteArgAs<std::string>(query_args.at("type"));
            if(!type_res)
            {
                response.setCode(http::Code::BadRequest)
                    .setBodyWithContentLength(json{
                        {"message", "Invalid event type"}
                    }.dump());
                co_return response;
            }

            if(!parse::parseEventType(*type_res).has_value())
            {
                response.setCode(http::Code::BadRequest)
                    .setBodyWithContentLength(json{
                        {"message", "Unknown event type"}
                    }.dump());
                co_return response;
            }

            feed_query.event_type = *type_res;
        }

        if(query_args.contains("include_unfinalized"))
        {
            auto include_res = parse::parseRouteArgAs<std::size_t>(query_args.at("include_unfinalized"));
            if(!include_res || (*include_res != 0u && *include_res != 1u))
            {
                response.setCode(http::Code::BadRequest)
                    .setBodyWithContentLength(json{
                        {"message", "Invalid include_unfinalized flag; expected 0 or 1"}
                    }.dump());
                co_return response;
            }
            feed_query.include_unfinalized = (*include_res == 1u);
        }

        const feed::FeedPage page = feed_repo.getFeedPage(feed_query);

        json output;
        output["limit"] = feed_query.limit;
        output["cursor"] = json::object();
        output["cursor"]["has_more"] = page.has_more;
        output["cursor"]["next_before"] = page.next_before_cursor.has_value()
            ? json(*page.next_before_cursor)
            : json(nullptr);
        output["items"] = json::array();
        for(const feed::FeedItem & item : page.items)
        {
            output["items"].push_back(json{
                {"feed_id", item.feed_id},
                {"event_type", item.event_type},
                {"status", item.status},
                {"visible", item.visible},
                {"tx_hash", item.tx_hash},
                {"block_number", item.block_number},
                {"tx_index", item.tx_index},
                {"log_index", item.log_index},
                {"history_cursor", item.history_cursor},
                {"created_at_ms", item.created_at_ms},
                {"updated_at_ms", item.updated_at_ms},
                {"projector_version", item.projector_version},
                {"payload", item.payload}
            });
        }

        response.setCode(http::Code::OK)
            .setBodyWithContentLength(output.dump());
        co_return response;
    }

    asio::awaitable<http::Response> OPTIONS_feedStream(const http::Request &, std::vector<server::RouteArg>, server::QueryArgsList)
    {
        http::Response response;
        response.setCode(http::Code::NoContent)
            .setVersion("HTTP/1.1")
            .setHeader(http::Header::AccessControlAllowOrigin, "*")
            .setHeader(http::Header::AccessControlAllowMethods, "GET, OPTIONS")
            .setHeader(http::Header::AccessControlAllowHeaders, "Content-Type")
            .setHeader(http::Header::AccessControlMaxAge, "600")
            .setHeader(http::Header::Connection, "close");
        co_return response;
    }

    namespace
    {
        constexpr std::chrono::milliseconds STREAM_POLL_INTERVAL{1000};
        constexpr std::chrono::seconds STREAM_DEADLINE_PER_WRITE{60};
        constexpr std::size_t STREAM_LIVE_LIMIT = 200;

        std::string formatDeltaFrame(const feed::StreamDelta & delta)
        {
            const json data{
                {"stream_seq", delta.stream_seq},
                {"event_type", delta.event_type},
                {"status", delta.status},
                {"feed_id", delta.feed_id},
                {"history_cursor", delta.history_cursor},
                {"created_at_ms", delta.created_at_ms},
                {"payload", delta.payload}
            };
            std::string out;
            out.reserve(256);
            out += std::format("id: {}\n", delta.stream_seq);
            out += std::format("event: {}\n", delta.event_type.empty() ? "unknown" : delta.event_type);
            out += std::format("data: {}\n\n", data.dump());
            return out;
        }

        std::string formatBadRequest(const std::string & message)
        {
            const std::string body = json{{"message", message}}.dump();
            return std::format(
                "HTTP/1.1 400 Bad Request\r\n"
                "Access-Control-Allow-Origin: *\r\n"
                "Connection: close\r\n"
                "Content-Type: application/json\r\n"
                "Content-Length: {}\r\n"
                "\r\n{}",
                body.size(),
                body);
        }

        asio::awaitable<bool> writeRaw(asio::ip::tcp::socket & sock, std::string data)
        {
            try
            {
                co_await asio::async_write(sock, asio::buffer(data), asio::use_awaitable);
                co_return true;
            }
            catch(const std::exception & e)
            {
                spdlog::debug("SSE write failed: {}", e.what());
                co_return false;
            }
            catch(...)
            {
                spdlog::debug("SSE write failed (unknown)");
                co_return false;
            }
        }
    }

    std::string buildFeedStreamSseReplay(
        feed::IFeedRepository & feed_repo,
        const feed::StreamQuery & stream_query)
    {
        const feed::StreamPage stream_page = feed_repo.getStreamPage(stream_query);

        std::string sse_body;
        sse_body.reserve(stream_page.deltas.size() * 256);
        sse_body += std::format(": min_available_seq={}\n\n", stream_page.min_available_seq);

        for(const feed::StreamDelta & delta : stream_page.deltas)
        {
            sse_body += formatDeltaFrame(delta);
        }

        const json meta{
            {"has_more", stream_page.has_more},
            {"last_seq", stream_page.last_seq.has_value() ? json(*stream_page.last_seq) : json(nullptr)},
            {"requested_since_seq", stream_query.since_seq},
            {"min_available_seq", stream_page.min_available_seq},
            {"replay_floor_seq", stream_page.replay_floor_seq},
            {"stale_since_seq", stream_page.stale_since_seq}
        };
        sse_body += "event: stream_meta\n";
        sse_body += std::format("data: {}\n\n", meta.dump());

        return sse_body;
    }

    asio::awaitable<void> GET_feedStream(
        asio::ip::tcp::socket & sock,
        const http::Request &,
        std::vector<server::RouteArg> route_args,
        server::QueryArgsList query_args,
        std::chrono::steady_clock::time_point & deadline,
        feed::IFeedRepository & feed_repo)
    {
        const auto refreshDeadline = [&deadline]()
        {
            deadline = std::chrono::steady_clock::now() + STREAM_DEADLINE_PER_WRITE;
        };

        if(!route_args.empty())
        {
            co_await writeRaw(sock, formatBadRequest("Invalid route arguments"));
            co_return;
        }

        feed::StreamQuery stream_query;
        stream_query.since_seq = 0;
        stream_query.limit = 200;

        if(query_args.contains("since_seq"))
        {
            auto since_res = parse::parseRouteArgAs<std::size_t>(query_args.at("since_seq"));
            if(!since_res || *since_res > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
            {
                co_await writeRaw(sock, formatBadRequest("Invalid since_seq cursor"));
                co_return;
            }
            stream_query.since_seq = static_cast<std::int64_t>(*since_res);
        }

        if(query_args.contains("limit"))
        {
            auto limit_res = parse::parseRouteArgAs<std::size_t>(query_args.at("limit"));
            if(!limit_res || *limit_res == 0 || *limit_res > MAX_STREAM_LIMIT)
            {
                co_await writeRaw(sock, formatBadRequest("Invalid stream limit"));
                co_return;
            }
            stream_query.limit = *limit_res;
        }

        // Response head — no Content-Length (body is unbounded). X-Accel-Buffering: no
        // disables nginx buffering if the server is fronted by a proxy.
        refreshDeadline();
        if(!co_await writeRaw(sock,
            "HTTP/1.1 200 OK\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Cache-Control: no-cache\r\n"
            "Connection: keep-alive\r\n"
            "Content-Type: text/event-stream; charset=utf-8\r\n"
            "X-Accel-Buffering: no\r\n"
            "\r\n"))
        {
            co_return;
        }

        // Initial replay: leading min_available_seq comment, all currently-available
        // deltas the cursor can see, and a stream_meta frame describing pagination.
        const feed::StreamPage replay_page = feed_repo.getStreamPage(stream_query);

        std::string replay_buf;
        replay_buf.reserve(replay_page.deltas.size() * 256);
        replay_buf += std::format(": min_available_seq={}\n\n", replay_page.min_available_seq);
        for(const feed::StreamDelta & delta : replay_page.deltas)
        {
            replay_buf += formatDeltaFrame(delta);
        }
        const json meta{
            {"has_more", replay_page.has_more},
            {"last_seq", replay_page.last_seq.has_value() ? json(*replay_page.last_seq) : json(nullptr)},
            {"requested_since_seq", stream_query.since_seq},
            {"min_available_seq", replay_page.min_available_seq},
            {"replay_floor_seq", replay_page.replay_floor_seq},
            {"stale_since_seq", replay_page.stale_since_seq}
        };
        replay_buf += "event: stream_meta\n";
        replay_buf += std::format("data: {}\n\n", meta.dump());

        if(!co_await writeRaw(sock, std::move(replay_buf)))
        {
            co_return;
        }
        refreshDeadline();

        // Anchor live tailing on the highest seq the replay observed. If the page
        // had no last_seq (no deltas), keep the requested cursor so nothing is missed.
        std::int64_t last_seq = replay_page.last_seq.value_or(stream_query.since_seq);
        spdlog::debug("SSE feed/stream: replay sent, tailing from stream_seq={}", last_seq);

        // Live loop: poll feed repository for new deltas once per STREAM_POLL_INTERVAL.
        // Always write to the socket each cycle (deltas or a keepalive comment) so
        // a disconnected client is detected within one poll cycle via write failure.
        asio::steady_timer poll_timer(co_await asio::this_coro::executor);

        while(true)
        {
            poll_timer.expires_after(STREAM_POLL_INTERVAL);
            try
            {
                co_await poll_timer.async_wait(asio::use_awaitable);
            }
            catch(...)
            {
                co_return;
            }

            feed::StreamQuery live_query;
            live_query.since_seq = last_seq;
            live_query.limit = STREAM_LIVE_LIMIT;
            const feed::StreamPage live_page = feed_repo.getStreamPage(live_query);

            spdlog::debug("SSE feed/stream poll: since_seq={} returned deltas={} page_last_seq={} min_available={}",
                last_seq,
                live_page.deltas.size(),
                live_page.last_seq.has_value() ? *live_page.last_seq : -1,
                live_page.min_available_seq);

            if(!live_page.deltas.empty())
            {
                std::string buf;
                buf.reserve(live_page.deltas.size() * 256);
                for(const feed::StreamDelta & delta : live_page.deltas)
                {
                    buf += formatDeltaFrame(delta);
                }
                if(live_page.last_seq.has_value())
                {
                    last_seq = *live_page.last_seq;
                }
                spdlog::debug("SSE feed/stream: emitting {} delta(s), advancing last_seq to {}", live_page.deltas.size(), last_seq);
                if(!co_await writeRaw(sock, std::move(buf)))
                {
                    co_return;
                }
            }
            else
            {
                // Write a keepalive comment on every idle poll so a closed client
                // connection is detected within one poll cycle via the write failure.
                if(!co_await writeRaw(sock, ":keepalive\n\n"))
                {
                    co_return;
                }
            }
            refreshDeadline();
        }
    }
}
