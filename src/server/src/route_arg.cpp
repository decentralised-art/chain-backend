#include "route_arg.hpp"

namespace dcn::server
{
    RouteArg::RouteArg(RouteArgDef def, std::string data)
    :   _def(std::move(def)),
        _data(std::move(data))
    {
        
    }

    RouteArgType RouteArg::getType() const
    {
        return _def.type;
    }

    const std::string & RouteArg::getData() const
    {
        return _data;
    }

    RouteArgRequirement RouteArg::getRequirement() const
    {
        return _def.requirement;
    }

    const std::vector<std::unique_ptr<RouteArgDef>> & RouteArg::getChildren() const
    {
        return _def.children;
    }
}

namespace dcn::parse
{
    server::RouteArgType parseRouteArgTypeFromString(const std::string & str)
    {
        if(str == std::format("{}", server::RouteArgType::character)) return server::RouteArgType::character;
        if(str == std::format("{}", server::RouteArgType::unsigned_integer)) return server::RouteArgType::unsigned_integer;
        if(str == std::format("{}", server::RouteArgType::base58)) return server::RouteArgType::base58;
        if(str == std::format("{}", server::RouteArgType::string)) return server::RouteArgType::string;
        if(str == std::format("{}", server::RouteArgType::array)) return server::RouteArgType::array;
        if(str == std::format("{}", server::RouteArgType::object)) return server::RouteArgType::object;

        return server::RouteArgType::Unknown;
    }

    parse::Result<server::RouteArgDef> parseRouteArgDefFromString(const std::string str)
    {   
        constexpr static const char start_delimeter = '<';
        constexpr static const char end_delimeter = '>';
        constexpr static const char optional_identifier = '~';
        
        const auto it_start = str.find(start_delimeter);
        if (it_start == std::string::npos) return std::unexpected(ParseError{ParseError::Kind::INVALID_VALUE, "Missing start delimeter"});
         
        const auto it_end = str.rfind(end_delimeter);
        if (it_end == std::string::npos) return std::unexpected(ParseError{ParseError::Kind::INVALID_VALUE, "Missing end delimeter"});
        
        assert(it_start < it_end);
        std::string arg = str.substr(it_start + 1, it_end - it_start - 1);
         
        if(arg.size() == 0)return std::unexpected(ParseError{ParseError::Kind::INVALID_VALUE, "Empty argument"});
            
        server::RouteArgRequirement requirement = server::RouteArgRequirement::required;
        server::RouteArgType type;
        
        if(arg.front() == optional_identifier)
        {
            // optional value
            requirement = server::RouteArgRequirement::optional;
            // remove optional identifier
            arg.erase(0, 1);
        }
        else
        {
            // required value
            requirement = server::RouteArgRequirement::required;
        }
        
        std::vector<std::unique_ptr<server::RouteArgDef>> additional_fields{};

        const auto it_array_start = arg.find(ARRAY_START_IDENTIFIER);
        const auto it_object_start = arg.find(OBJECT_START_IDENTIFIER);
        
        if(it_array_start != std::string::npos)
        {
            const auto it_array_end = arg.rfind(ARRAY_END_IDENTIFIER);
            if(it_array_end == std::string::npos)
            {
                spdlog::error("parseRouteArg - cannot find array end identifier : {}", arg);
                return std::unexpected(ParseError{ParseError::Kind::INVALID_VALUE, "Cannot find array end identifier"});
            }

            // array type
            type = server::RouteArgType::array;
            // remove array identifiers
            arg = arg.substr(it_array_start + 1, it_array_end - it_array_start - 1);
            
            // if its an array we need to fetch its type <[...]>
            const auto array_type = parseRouteArgDefFromString(arg);
            if(!array_type)
            {
                spdlog::error("parseRouteArg - cannot find array type definition : {}", arg);
                return std::unexpected(ParseError{ParseError::Kind::TYPE_MISMATCH, "Cannot find array type definition"});
            }

            if(array_type->requirement == server::RouteArgRequirement::optional)
            {
                spdlog::error("parseRouteArg - array type cannot be optional : {}", arg);
                return std::unexpected(ParseError{ParseError::Kind::TYPE_MISMATCH, "Array type cannot be optional"});
            }

            additional_fields.emplace_back(std::make_unique<server::RouteArgDef>(std::move(*array_type)));
        }
        else if(it_object_start != std::string::npos)
        {
            const auto it_object_end = arg.rfind(OBJECT_END_IDENTIFIER);
            if(it_object_end == std::string::npos)
            {
                spdlog::error("parseRouteArg - cannot find object end identifier : {}", arg);
                return std::unexpected(ParseError{ParseError::Kind::INVALID_VALUE, "Cannot find object end identifier"});
            }

            // object type
            type = server::RouteArgType::object;

            // remove object identifiers
            arg = arg.substr(it_object_start + 1, it_object_end - it_object_start - 1);

            // if its an object we need to fetch its fields <(...)>
            std::string & object_fields = arg;
            if(object_fields.empty())
            {
                spdlog::error("parseRouteArg - cannot find object fields : {}", arg);
                return std::unexpected(ParseError{ParseError::Kind::INVALID_VALUE, "Cannot find object fields"});
            }

            // we need to split the object fields by comma
            std::vector<std::string> fields;
            std::size_t pos = 0;
            while ((pos = object_fields.find(OBJECT_FIELDS_DELIMETER)) != std::string::npos) {
                fields.push_back(object_fields.substr(0, pos));
                object_fields.erase(0, pos + 1);
            }
            fields.push_back(object_fields); // add the last field
            
            if(fields.size() == 0)
            {
                spdlog::error("parseRouteArg - cannot find object fields : {}", arg);
                return std::unexpected(ParseError{ParseError::Kind::INVALID_VALUE, "Cannot find object fields"});
            }

            // for every field we need to parse it
            for(const auto & field : fields)
            {
                const auto field_type = parseRouteArgDefFromString(field);
                if(!field_type)
                {
                    spdlog::error("parseRouteArg - cannot find object field type definition : {}", field);
                    return std::unexpected(ParseError{ParseError::Kind::TYPE_MISMATCH, "Cannot find object field type definition"});
                }

                if(field_type->requirement == server::RouteArgRequirement::optional)
                {
                    spdlog::error("parseRouteArg - object field type cannot be optional : {}", arg);
                    return std::unexpected(ParseError{ParseError::Kind::TYPE_MISMATCH, "Object field type cannot be optional"});
                }

                additional_fields.emplace_back(std::make_unique<server::RouteArgDef>(std::move(*field_type)));
            }
        }
        else 
        {
            // normal type
            type = parseRouteArgTypeFromString(arg);
        }

        if(type == server::RouteArgType::Unknown)
        {
            spdlog::error("parseRouteArg - cannot find type definition : {}", arg);
            return std::unexpected(ParseError{ParseError::Kind::TYPE_MISMATCH, "Cannot find type definition"});
        }

        return server::RouteArgDef(type, requirement, std::move(additional_fields));
    }

    template<>
    parse::Result<std::size_t> parseRouteArgAs<std::size_t>(const server::RouteArg & arg) 
    {
        if(arg.getType() != server::RouteArgType::unsigned_integer)return std::unexpected(ParseError{ParseError::Kind::TYPE_MISMATCH});
        
        try {
            std::size_t pos = 0;
            const std::size_t val = std::stoull(arg.getData(), &pos);

            // Ensure the whole string was parsed
            if(pos != arg.getData().size())
                return std::unexpected(ParseError{ParseError::Kind::INVALID_VALUE});

            // Ensure the value fits in size_t
            if (val > std::numeric_limits<std::size_t>::max())
                return std::unexpected(ParseError{ParseError::Kind::OUT_OF_RANGE});

            return val;
        }
        catch(const std::out_of_range &)
        {
            return std::unexpected(ParseError{ParseError::Kind::OUT_OF_RANGE});
        }
        catch(const std::invalid_argument &)
        {
            return std::unexpected(ParseError{ParseError::Kind::INVALID_VALUE});
        }
        catch(...)
        {
            return std::unexpected(ParseError{ParseError::Kind::UNKNOWN});
        }
    }


    template<>
    parse::Result<std::uint32_t> parseRouteArgAs<std::uint32_t>(const server::RouteArg & arg) 
    {
        if(arg.getType() != server::RouteArgType::unsigned_integer)return std::unexpected(ParseError{ParseError::Kind::TYPE_MISMATCH});

        try {
            std::size_t pos = 0;
            const unsigned long long val = std::stoull(arg.getData(), &pos);

            // Ensure the whole string was parsed
            if (pos != arg.getData().size())
                return std::unexpected(ParseError{ParseError::Kind::INVALID_VALUE});
            
            // Ensure the value fits in uint32_t
            if (val > std::numeric_limits<std::uint32_t>::max())
                return std::unexpected(ParseError{ParseError::Kind::OUT_OF_RANGE});

            return static_cast<std::uint32_t>(val);
        } 
        catch(const std::out_of_range &) 
        {
            return std::unexpected(ParseError{ParseError::Kind::OUT_OF_RANGE});
        }
        catch(const std::invalid_argument &)
        {
            return std::unexpected(ParseError{ParseError::Kind::INVALID_VALUE});
        }
        catch (...)
        {
            return std::unexpected(ParseError{ParseError::Kind::UNKNOWN});
        }
    }

    template<>
    parse::Result<std::string> parseRouteArgAs<std::string>(const server::RouteArg & arg) 
    {
        if(arg.getType() != server::RouteArgType::string)return std::unexpected(ParseError{ParseError::Kind::TYPE_MISMATCH});
        return arg.getData();
    }
}