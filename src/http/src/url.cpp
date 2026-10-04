#include "url.hpp"
#include "utils.hpp"

#include <algorithm>
#include <charconv>

namespace dcn::http
{
    std::optional<std::string> canonicalOrigin(const std::string_view origin)
    {
        const std::size_t scheme_end = origin.find("://");
        if(scheme_end == std::string_view::npos) return std::nullopt;
        const std::string scheme = utils::toLower(std::string(origin.substr(0, scheme_end)));
        if(scheme != "http" && scheme != "https") return std::nullopt;

        std::string_view authority = origin.substr(scheme_end + 3);
        std::optional<std::string_view> port_text;
        if(const std::size_t colon = authority.find(':'); colon != std::string_view::npos)
        {
            port_text = authority.substr(colon + 1);
            authority = authority.substr(0, colon);
        }

        // Dot-separated labels of letters, digits and inner hyphens: a DNS name or an IPv4 address.
        const std::string host = utils::toLower(std::string(authority));
        if(host.empty() || host.size() > 253) return std::nullopt;
        for(std::size_t begin = 0; begin <= host.size();)
        {
            const std::size_t end = std::min(host.find('.', begin), host.size());
            const std::string_view label = std::string_view(host).substr(begin, end - begin);
            if(label.empty() || label.size() > 63 || label.front() == '-' || label.back() == '-') return std::nullopt;
            if(!std::ranges::all_of(label, [](const char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; }))
            {
                return std::nullopt;
            }
            begin = end + 1;
        }

        std::string canonical = scheme + "://" + host;
        if(port_text)
        {
            unsigned port = 0;
            const char * const first = port_text->data();
            const char * const last = first + port_text->size();
            const auto [parsed_end, error] = std::from_chars(first, last, port);
            if(port_text->empty() || port_text->size() > 5 || error != std::errc{} || parsed_end != last
                || port == 0 || port > 65535)
            {
                return std::nullopt;
            }
            if(port != (scheme == "https" ? 443u : 80u)) canonical += std::format(":{}", port);
        }
        return canonical;
    }

    URL::URL(const std::string & url) 
    : _url(url)
    {
    }

    URL::URL(std::string && url)
    : _url(std::move(url))
    {

    }

    URL::URL(const char * url)
    : URL(std::string(url))
    {
    }

    bool URL::operator==(const URL& other) const
    {
        return _url == other._url;
    }

    std::string URL::getFullPath() const
    {
        return _url.substr(0, _url.find('?'));
    }

    std::string URL::getQuery() const
    {
        size_t start = _url.find('?');
        if (start != std::string::npos)
        {
            size_t end = _url.find('#', start);
            return _url.substr(start + 1, end - start - 1);
        }
        return "";
    }

    std::string URL::getPathModule() const
    {
        const std::string full_path = getFullPath();
        size_t start = full_path.find('/');
        size_t end = full_path.find('/', start + 1);

        if(start == std::string::npos)return "";

        return full_path.substr(start, end - start);
    }

    std::string URL::getPathInfo() const
    {
        size_t start = _url.find('/', _url.find('/') + 1);
        size_t end = _url.find('?');
        
        if(start == std::string::npos)return "";

        return _url.substr(start, end - start);
    }
}

namespace dcn::http
{
    std::vector<std::string> splitPathSegments(const std::string path)
    {
        std::vector<std::string> path_parts;
        if(path.empty())return path_parts;

        static const char path_delimeter = '/';

        std::stringstream ss(path);
        std::string segment;

        // first segment must be empty to ensure that path starts with /
        if(!std::getline(ss, segment, path_delimeter) || !segment.empty())return path_parts;

        while (std::getline(ss, segment, path_delimeter)) 
        {
            path_parts.push_back(segment);
        }

        return path_parts;
    }

    std::optional<std::string> decodeQueryComponent(const std::string_view component)
    {
        const auto hex_value = [](const char c) -> int
        {
            if(c >= '0' && c <= '9') return c - '0';
            if(c >= 'a' && c <= 'f') return c - 'a' + 10;
            if(c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };

        std::string decoded;
        decoded.reserve(component.size());
        for(std::size_t i = 0; i < component.size(); ++i)
        {
            if(component[i] == '+')
            {
                decoded += ' ';
                continue;
            }
            if(component[i] != '%')
            {
                decoded += component[i];
                continue;
            }
            if(i + 2 >= component.size()) return std::nullopt;
            const int high = hex_value(component[i + 1]);
            const int low = hex_value(component[i + 2]);
            if(high < 0 || low < 0) return std::nullopt;
            decoded += static_cast<char>(high * 16 + low);
            i += 2;
        }
        return decoded;
    }

    absl::flat_hash_map<std::string, std::string> splitQuerySegments(const std::string& query)
    {
        absl::flat_hash_map<std::string, std::string> query_map;

        if (query.empty()) return query_map;

        std::stringstream ss(query);
        std::string segment;

        while (std::getline(ss, segment, '&'))
        {
            auto pos = segment.find('=');
            if (pos == std::string::npos) continue; // skip invalid segments

            auto key = decodeQueryComponent(std::string_view(segment).substr(0, pos));
            auto value = decodeQueryComponent(std::string_view(segment).substr(pos + 1));
            if (!key || !value) continue; // skip malformed percent-encoding

            if (!key->empty())
                query_map[std::move(*key)] = std::move(*value);
        }

        return query_map;
    }
}
