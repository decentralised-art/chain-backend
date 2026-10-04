#pragma once

#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <sstream>

#include <absl/container/flat_hash_map.h>

namespace dcn::http
{
    class URL
    {
        public:
            URL() = default;
            URL(const std::string & url);
            URL(std::string && url);
            URL(const char * url);

            URL(const URL &) = default;
            URL(URL &&) = default;
            ~URL() = default;

            URL & operator=(const URL &) = default;
            URL & operator=(URL &&) = default;

            bool operator==(const URL& other) const;

            /**
             * @brief Returns the full path of the URL, including the query string.
             * @return The full path of the URL.
             */
            std::string getFullPath() const;

            /**
             * @brief Returns the path module of the URL.
             * @return The path module of the URL.
             */
            std::string getPathModule() const;

            /**
             * @brief Returns the path info of the URL.
             * @return The path info of the URL.
             */
            std::string getPathInfo() const;

            /**
             * @brief Returns the query string of the URL.
             * @return The query string of the URL.
             */
            std::string getQuery() const;

        private:
            std::string _url;
    };

    /**
     * @brief Combines hash values for a RouteKey object.
     *
     * @tparam H The hash state type.
     * @param h The initial hash state.
     * @param url The URL object whose attributes will be hashed.
     * @return A combined hash state
     */
    template <typename H>
    inline H AbslHashValue(H h, const URL & url) {
        return H::combine(std::move(h), url.getFullPath());
    }

    std::vector<std::string> splitPathSegments(const std::string path);

    /**
     * @brief `component` of a query with application/x-www-form-urlencoded escapes undone:
     * `+` is a space and `%XY` the byte with hex value XY.
     * @return nullopt for a `%` not followed by two hex digits.
     */
    std::optional<std::string> decodeQueryComponent(std::string_view component);

    /// The `key=value` pairs of `query`, decoded with decodeQueryComponent. Pairs without `=`
    /// or with malformed escapes are skipped.
    absl::flat_hash_map<std::string, std::string> splitQuerySegments(const std::string& query);

    /**
     * @brief The serialized origin a browser reports for `origin` (as `window.location.origin`
     * or the `Origin` header): lower-case scheme and host, and the port only when it is not
     * the scheme's default.
     *
     * Accepts `http(s)://host[:port]` with no path, where host is a DNS name or IPv4 address.
     * @return nullopt for anything else.
     */
    std::optional<std::string> canonicalOrigin(std::string_view origin);
}

template <>
struct std::formatter<dcn::http::URL> : std::formatter<std::string> {
  auto format(const dcn::http::URL & url, format_context& ctx) const {
    return formatter<string>::format(
      std::format("{}", url.getFullPath()), ctx);
  }
};