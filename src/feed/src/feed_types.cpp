#include "feed_types.hpp"

namespace dcn::feed
{
    parse::Result<CursorKey> parseHistoryCursor(const std::string & cursor)
    {
        std::size_t p0 = cursor.find(':');
        if(p0 == std::string::npos)
        {
            return std::unexpected(parse::ParseError{parse::ParseError::Kind::INVALID_VALUE});
        }

        std::size_t p1 = cursor.find(':', p0 + 1);
        if(p1 == std::string::npos)
        {
            return std::unexpected(parse::ParseError{parse::ParseError::Kind::INVALID_VALUE});
        }

        std::size_t p2 = cursor.find(':', p1 + 1);
        if(p2 == std::string::npos)
        {
            return std::unexpected(parse::ParseError{parse::ParseError::Kind::INVALID_VALUE});
        }

        std::size_t p3 = cursor.find(':', p2 + 1);
        if(p3 == std::string::npos)
        {
            return std::unexpected(parse::ParseError{parse::ParseError::Kind::INVALID_VALUE});
        }

        CursorKey key{};
        try
        {
            const std::string first_token = cursor.substr(0, p0);
            const bool has_created_prefix = first_token.size() > 1 && first_token.front() == 'c';

            if(!has_created_prefix)
            {
                return std::unexpected(parse::ParseError{parse::ParseError::Kind::INVALID_VALUE});
            }

            key.created_at_ms = std::stoll(first_token.substr(1));
            key.block_number = std::stoll(cursor.substr(p0 + 1, p1 - (p0 + 1)));
            key.tx_index = std::stoll(cursor.substr(p1 + 1, p2 - (p1 + 1)));
            key.feed_id = cursor.substr(p2 + 1);

            const std::size_t f0 = key.feed_id.find(':');
            if(f0 == std::string::npos)
            {
                return std::unexpected(parse::ParseError{parse::ParseError::Kind::INVALID_VALUE});
            }

            const std::size_t f1 = key.feed_id.find(':', f0 + 1);
            if(f1 == std::string::npos)
            {
                return std::unexpected(parse::ParseError{parse::ParseError::Kind::INVALID_VALUE});
            }

            key.chain_namespace = key.feed_id.substr(0, f0);
            key.chain_id = std::stoull(key.feed_id.substr(f0 + 1, f1 - (f0 + 1)));
        }
        catch(...)
        {
            return std::unexpected(parse::ParseError{parse::ParseError::Kind::INVALID_VALUE});
        }

        if(key.chain_namespace.empty() || key.feed_id.empty())
        {
            return std::unexpected(parse::ParseError{parse::ParseError::Kind::INVALID_VALUE});
        }

        return key;
    }
}
