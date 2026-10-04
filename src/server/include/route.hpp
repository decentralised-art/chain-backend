#pragma once
#include <string>
#include <functional>
using namespace std::placeholders;

#include <chrono>
#include <memory>
#include <utility>
#include <vector>
#include <cassert>

#include "native.h"
#include <asio.hpp>
#include <absl/hash/hash.h>
#include <absl/container/flat_hash_map.h>

#include "http.hpp"

#include "route_arg.hpp"
#include "route_key.hpp"

namespace dcn::server
{
    using QueryArgsList = absl::flat_hash_map<std::string, RouteArg>;

    /**
     * @brief Streaming handler signature.
     *
     * Long-lived handlers (SSE, chunked transfer, etc.) own the socket for the full
     * lifetime of the request. They are responsible for writing the entire HTTP
     * response — status line, headers, and body — and for refreshing `deadline`
     * so the connection-level watchdog does not trip during healthy streaming.
     */
    using StreamingHandlerDefinition = std::function<asio::awaitable<void>(
        asio::ip::tcp::socket &,
        const dcn::http::Request &,
        std::vector<RouteArg>,
        QueryArgsList,
        std::chrono::steady_clock::time_point &)>;

    /**
     * @brief A class representing a route handler function.
     *
     * This class is used to store and execute route handler function.
     */
    class RouteHandlerFunc
    {
        public:
            enum class Kind
            {
                Response,
                Streaming
            };

        private:
            class Base
            {
                public:
                    virtual ~Base() = default;
            };

            template<typename... Args>
            struct Wrapper : public Base
            {
                Wrapper(std::function<asio::awaitable<http::Response>(Args...)> func)
                : function(std::move(func))
                {}

                std::function<asio::awaitable<http::Response>(Args...)> function;
            };

        public:

            template<typename... Args>
            RouteHandlerFunc(std::function<asio::awaitable<http::Response>(Args...)> func)
            : _kind(Kind::Response),
              _base(std::make_unique<Wrapper<Args...>>(std::move(func)))
            {}

            RouteHandlerFunc(StreamingHandlerDefinition func)
            : _kind(Kind::Streaming),
              _streaming(std::move(func))
            {}

            RouteHandlerFunc(RouteHandlerFunc && other) = default;

            Kind kind() const { return _kind; }

            template<typename... Args>
            asio::awaitable<http::Response> operator()(Args && ... args) const
            {
                Wrapper<Args...>* wrapper_ptr = dynamic_cast<Wrapper<Args...>*>(_base.get());

                if(wrapper_ptr)
                {
                    co_return co_await wrapper_ptr->function(std::forward<Args>(args)...);
                }
                else
                {
                    throw std::runtime_error("Invalid arguments to function object call!");
                }
            }

            asio::awaitable<void> invokeStreaming(
                asio::ip::tcp::socket & sock,
                const http::Request & request,
                std::vector<RouteArg> route_args,
                QueryArgsList query_args,
                std::chrono::steady_clock::time_point & deadline) const
            {
                if(_kind != Kind::Streaming || !_streaming)
                {
                    throw std::runtime_error("RouteHandlerFunc::invokeStreaming called on non-streaming handler");
                }
                co_await _streaming(sock, request, std::move(route_args), std::move(query_args), deadline);
            }

        private:
            Kind _kind;
            std::unique_ptr<Base> _base;
            StreamingHandlerDefinition _streaming;
    };

    /**
     * @brief A class representing a router for handling HTTP requests.
     * 
     * This class is used to store and execute set of route handler functions.
     */
    class Router
    {
        public:
            Router() = default;
            ~Router() = default;
            
            void addRoute(RouteKey route, RouteHandlerFunc handler);

            std::tuple<const RouteHandlerFunc *, std::vector<RouteArg>,  QueryArgsList> findRoute(const http::Request & request) const;
        protected:
            std::tuple<bool, std::vector<RouteArg>, QueryArgsList> doesRouteMatch(
                    const RouteKey & route,
                    const http::Method & request_method,
                    const std::string & module_path,
                    const std::vector<std::string> & request_path_info_segments,
                    const absl::flat_hash_map<std::string, std::string> request_query_segments) const;

        private:
            absl::flat_hash_map<RouteKey, RouteHandlerFunc> _routes;
    };
}
