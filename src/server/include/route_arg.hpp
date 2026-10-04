#pragma once

#include <string>
#include <format>
#include <cassert>
#include <tuple>
#include <vector>
#include <list>
#include <memory>
#include <type_traits>

#include <spdlog/spdlog.h>

#include "parse_error.hpp"

namespace dcn::server
{
    /**
     * @brief Enum to represent the type of a route argument.
     * 
     * This enum represents the possible types of a route argument.
     */
    enum class RouteArgType
    {
        // Unknown
        Unknown = 0,

        character,
        unsigned_integer,
        base58,
        string,
        array,
        object
    };

    /**
     * @brief Enum to represent the requirement of a route argument.
     * 
     * This enum represents the possible requirements of a route argument.
     */
    enum class RouteArgRequirement
    {
        // Unknown
        Unknown = 0,

        optional,
        required
    };

    /**
     * @brief A pair of RouteArgType and RouteArgRequirement.
     * 
     * This type represents a struct of RouteArgType, RouteArgRequirement and std::vector<RouteArgType>.
     * last value is used in case of array or object
     */
    struct RouteArgDef
    {
        RouteArgDef(RouteArgType type, RouteArgRequirement requirement)
            : type(type), requirement(requirement) {}
        
        RouteArgDef(RouteArgType type, RouteArgRequirement requirement, std::vector<std::unique_ptr<RouteArgDef>> children)
            : type(type), requirement(requirement), children(std::move(children)) {}

        // Copy constructor (deep copy)
        RouteArgDef(const RouteArgDef& other)
            : type(other.type), requirement(other.requirement)
        {
            children.reserve(other.children.size());
            for (const auto& child : other.children)
            {
                if (child)
                    children.emplace_back(std::make_unique<RouteArgDef>(*child));
                else
                    children.emplace_back(nullptr);
            }
        }

        // Move constructor
        RouteArgDef(RouteArgDef&& other) noexcept
            : type(std::move(other.type)), requirement(std::move(other.requirement)), children(std::move(other.children)) {}

        RouteArgType type;
        RouteArgRequirement requirement;
        std::vector<std::unique_ptr<RouteArgDef>> children;
    };

    /**
     * @brief A class representing a route argument.
     * 
     * This class represents a route argument, which is a pair of RouteArgType and RouteArgRequirement.
     * It also holds the data associated with the argument.
     */
    class RouteArg
    {
        public:
            RouteArg(RouteArgDef def, std::string data);

            /**
             * @brief Gets the type of the route argument.
             * 
             * @return The type of the route argument.
             */
            RouteArgType getType() const;

            /**
             * @brief Gets the data associated with the route argument.
             * 
             * @return The data associated with the route argument.
             */
            const std::string & getData() const;

            /**
             * @brief Gets the requirement of the route argument.
             * 
             * @return The requirement of the route argument.
             */
            RouteArgRequirement getRequirement() const;

            /**
             * @brief Gets the children of the route argument.
             * 
             * @return The children of the route argument.
             */
            const std::vector<std::unique_ptr<RouteArgDef>> & getChildren() const;

        private:

            RouteArgDef _def;
            std::string _data;
    };
}

namespace dcn::parse
{
    constexpr static const char ARRAY_START_IDENTIFIER = '[';
    constexpr static const char ARRAY_END_IDENTIFIER = ']';

    constexpr static const unsigned int MAX_OBJECT_FIELDS = 5;
    constexpr static const char OBJECT_START_IDENTIFIER = '(';
    constexpr static const char OBJECT_END_IDENTIFIER = ')';
    constexpr static const char OBJECT_FIELDS_DELIMETER = ';';

    // Base check for presence of value_type and iterator
    template<typename T>
    concept HasValueTypeAndIterator = requires {
        typename T::value_type;
        typename T::iterator;
    };

    // General concept for STL-like containers
    template<typename T>
    concept IsSequenceContainer = HasValueTypeAndIterator<T> &&
    (
        std::is_array<T>::value ||
        std::is_same_v<T, std::vector<typename T::value_type>> ||
        std::is_same_v<T, std::list<typename T::value_type>>
    );

    template<typename T>
    struct is_tuple_like_impl : std::false_type {};
    
    template<typename... Ts>
    struct is_tuple_like_impl<std::tuple<Ts...>> : std::true_type {};
    
    template<typename T1, typename T2>
    struct is_tuple_like_impl<std::pair<T1, T2>> : std::true_type {};
    
    template<typename T>
    concept IsTupleLike =   
        is_tuple_like_impl<std::remove_cv_t<std::remove_reference_t<T>>>::value 
        && std::tuple_size<T>::value > 0 
        && std::tuple_size<T>::value < MAX_OBJECT_FIELDS;

    /**
     * @brief Parses a string to a RouteArgType.
     * 
     * This function takes a string representation of a `RouteArgType` and converts it to the corresponding enum value.
     * If the string does not match any known `RouteArgType`, it returns `RouteArgType::Unknown`.
     * 
     * @param str The `string` to parse.
     * @return The corresponding RouteArgType.
     */
    server::RouteArgType parseRouteArgTypeFromString(const std::string & str);
    
    /**
     * @brief Parses a string to a RouteArgDef.
     * 
     * This function takes a string representation of a `RouteArgDef` and converts it to the corresponding enum value.
     * 
     * @param str The `string` to parse.
     * @return The corresponding RouteArgDef.
     */
    parse::Result<server::RouteArgDef> parseRouteArgDefFromString(const std::string str);

    template<class T>
    requires (!IsSequenceContainer<T> && !IsTupleLike<T>)
    parse::Result<T> parseRouteArgAs(const server::RouteArg & arg);

    /**
     * @brief Parses a `RouteArg` as a unsigned integer.
     * 
     * This function takes a `RouteArg` and attempts to parse it as a unsigned integer.
     * 
     * @param arg The `RouteArg` to parse.
     */
    template<>
    parse::Result<std::size_t> parseRouteArgAs<std::size_t>(const server::RouteArg & arg);

    /**
     * @brief Parses a `RouteArg` as a 32bit unsigned integer.
     * 
     * This function takes a `RouteArg` and attempts to parse it as a 32bit unsigned integer.
     * 
     * @param arg The `RouteArg` to parse.
     */
    template<>
    parse::Result<std::uint32_t> parseRouteArgAs<std::uint32_t>(const server::RouteArg & arg);

    /**
     * @brief Parses a `RouteArg` as a string.
     * 
     * This function takes a `RouteArg` and attempts to parse it as a `string`.
     * 
     * @param arg The `RouteArg` to parse.
     */
    template<>
    parse::Result<std::string> parseRouteArgAs<std::string>(const server::RouteArg & arg);


    template <IsTupleLike TupleT, std::size_t Size>
    struct TupleParser;

    template <IsTupleLike TupleT>
    struct TupleParser<TupleT, 2>
    {
        parse::Result<TupleT> operator()(const std::vector<std::unique_ptr<dcn::server::RouteArgDef>> &defs, const std::vector<std::string> & values_str)
        {
            if(values_str.size() != std::tuple_size<TupleT>::value) return std::unexpected(ParseError{ParseError::Kind::INVALID_VALUE});
            if(defs.size() != std::tuple_size<TupleT>::value) return std::unexpected(ParseError{ParseError::Kind::INVALID_VALUE});

            auto t0 = parseRouteArgAs<typename std::tuple_element<0, TupleT>::type>(server::RouteArg(*defs.at(0), values_str.at(0)));
            if(!t0) return std::unexpected(t0.error());

            auto t1 = parseRouteArgAs<typename std::tuple_element<0, TupleT>::type>(server::RouteArg(*defs.at(1), values_str.at(1)));
            if(!t1) return std::unexpected(t1.error());
            
            return std::make_tuple(*t0, *t1);
        }
    };

    template <IsTupleLike TupleT>
    struct TupleParser<TupleT, 3>
    {
        parse::Result<TupleT> operator()(const std::vector<std::unique_ptr<dcn::server::RouteArgDef>> &defs, const std::vector<std::string> & values_str)
        {
            if(values_str.size() != std::tuple_size<TupleT>::value) return std::unexpected(ParseError{ParseError::Kind::INVALID_VALUE});
            if(defs.size() != std::tuple_size<TupleT>::value) return std::unexpected(ParseError{ParseError::Kind::INVALID_VALUE});

            auto t0 = parseRouteArgAs<typename std::tuple_element<0, TupleT>::type>(server::RouteArg(*defs.at(0), values_str.at(0)));
            if(!t0) return std::unexpected(t0.error());

            auto t1 = parseRouteArgAs<typename std::tuple_element<0, TupleT>::type>(server::RouteArg(*defs.at(1), values_str.at(1)));
            if(!t1) return std::unexpected(t1.error());

            auto t2 = parseRouteArgAs<typename std::tuple_element<0, TupleT>::type>(server::RouteArg(*defs.at(2), values_str.at(2)));
            if(!t2) return std::unexpected(t2.error());
            
            return std::make_tuple(*t0, *t1, *t2);
        }
    };

    template <IsTupleLike TupleT>
    struct TupleParser<TupleT, 4>
    {
        parse::Result<TupleT> operator()(const std::vector<std::unique_ptr<dcn::server::RouteArgDef>> &defs, const std::vector<std::string> & values_str)
        {
            if(values_str.size() != std::tuple_size<TupleT>::value) return std::unexpected(ParseError{ParseError::Kind::INVALID_VALUE});
            if(defs.size() != std::tuple_size<TupleT>::value) return std::unexpected(ParseError{ParseError::Kind::INVALID_VALUE});

            auto t0 = parseRouteArgAs<typename std::tuple_element<0, TupleT>::type>(server::RouteArg(*defs.at(0), values_str.at(0)));
            if(!t0) return std::unexpected(t0.error());

            auto t1 = parseRouteArgAs<typename std::tuple_element<0, TupleT>::type>(server::RouteArg(*defs.at(1), values_str.at(1)));
            if(!t1) return std::unexpected(t1.error());

            auto t2 = parseRouteArgAs<typename std::tuple_element<0, TupleT>::type>(server::RouteArg(*defs.at(2), values_str.at(2)));
            if(!t2) return std::unexpected(t2.error());

            auto t3 = parseRouteArgAs<typename std::tuple_element<0, TupleT>::type>(server::RouteArg(*defs.at(3), values_str.at(3)));
            if(!t3) return std::unexpected(t3.error());
            
            return std::make_tuple(*t0, *t1, *t2, *t3);
        }
    };


    template<IsTupleLike TupleT>
    parse::Result<TupleT> parseRouteArgAs(const server::RouteArg& arg)
    {
        if(arg.getType() != server::RouteArgType::object) return std::unexpected(ParseError{ParseError::Kind::TYPE_MISMATCH});
        if(std::tuple_size<TupleT>::value == 0) return std::unexpected(ParseError{ParseError::Kind::INVALID_VALUE});
        if(arg.getChildren().size() != std::tuple_size<TupleT>::value) return std::unexpected(ParseError{ParseError::Kind::INVALID_VALUE});
        
        std::string data = arg.getData();
        if(data.empty()) return std::unexpected(ParseError{ParseError::Kind::INVALID_VALUE});
        if(data.front() != OBJECT_START_IDENTIFIER) return std::unexpected(ParseError{ParseError::Kind::INVALID_VALUE});
        if(data.back() != OBJECT_END_IDENTIFIER) return std::unexpected(ParseError{ParseError::Kind::INVALID_VALUE});
        
        // remove delimiters
        data = data.substr(1, data.size() - 2);

        // split arg into fields
        std::vector<std::string> values_str;
        std::size_t begin = 0;
        std::size_t end = 0;

        while ((end = data.find(OBJECT_FIELDS_DELIMETER, begin)) != std::string::npos) {
            values_str.push_back(data.substr(begin, (end - begin)));
            begin = end + 1;
        }
        // add the last value
        values_str.push_back(data.substr(begin));

        return TupleParser<TupleT, std::tuple_size<TupleT>::value>{}(arg.getChildren(), values_str);
    }

    template<IsSequenceContainer ContainerT>
    parse::Result<ContainerT> parseRouteArgAs(const server::RouteArg& arg)
    {
        using T = typename ContainerT::value_type;

        if(arg.getType() != server::RouteArgType::array) return std::unexpected(ParseError{ParseError::Kind::TYPE_MISMATCH});
        if(arg.getChildren().size() != 1) return std::unexpected(ParseError{ParseError::Kind::INVALID_VALUE});
        if(arg.getChildren().at(0) == nullptr) return std::unexpected(ParseError{ParseError::Kind::INVALID_VALUE});

        std::string data = arg.getData();
        if(data.empty()) return std::unexpected(ParseError{ParseError::Kind::INVALID_VALUE});
        if(data.front() != ARRAY_START_IDENTIFIER) return std::unexpected(ParseError{ParseError::Kind::INVALID_VALUE});
        if(data.back() != ARRAY_END_IDENTIFIER) return std::unexpected(ParseError{ParseError::Kind::INVALID_VALUE});

        // remove delimiters
        data = data.substr(1, data.size() - 2);

        // split arg into values
        constexpr static const char array_values_delimeter = ',';

        std::vector<std::string> values_str;
        std::size_t begin = 0;
        std::size_t end = 0;

        while ((end = data.find(array_values_delimeter, begin)) != std::string::npos) {
            values_str.push_back(data.substr(begin, (end - begin)));
            begin = end + 1;
        }
        // add the last value
        values_str.push_back(data.substr(begin));

        ContainerT values;
        const auto & array_type = *arg.getChildren().at(0);

        for (const auto & value_str : values_str)
        {
            server::RouteArg array_value = server::RouteArg(array_type, value_str);
            const auto parsed_value = parseRouteArgAs<T>(array_value);
            if(!parsed_value)return std::unexpected(parsed_value.error());

            values.emplace_back(*parsed_value);
        }

        return values;
    }
}

template <>
struct std::formatter<dcn::server::RouteArgType> : std::formatter<std::string> {
  auto format(const dcn::server::RouteArgType & arg_type, format_context& ctx) const {
    switch(arg_type)
    {
        case dcn::server::RouteArgType::character:           return formatter<string>::format("char", ctx);
        case dcn::server::RouteArgType::unsigned_integer:    return formatter<string>::format("uint", ctx);
        case dcn::server::RouteArgType::string:              return formatter<string>::format("string", ctx);
        case dcn::server::RouteArgType::base58:              return formatter<string>::format("base58", ctx);
        case dcn::server::RouteArgType::array:               return formatter<string>::format("array", ctx);
        case dcn::server::RouteArgType::object:              return formatter<string>::format("object", ctx);

        // Unknown
        case dcn::server::RouteArgType::Unknown:             return formatter<string>::format("Unknown", ctx);
    }
    return formatter<string>::format("Unknown", ctx);
  }
};

template <>
struct std::formatter<dcn::server::RouteArgRequirement> : std::formatter<std::string> {
  auto format(const dcn::server::RouteArgRequirement & req, format_context& ctx) const {
    switch(req)
    {
        case dcn::server::RouteArgRequirement::required:    return formatter<string>::format("required", ctx);
        case dcn::server::RouteArgRequirement::optional:    return formatter<string>::format("(optional)", ctx);

        // Unknown
        case dcn::server::RouteArgRequirement::Unknown:      return formatter<string>::format("Unknown", ctx);
    }
    return formatter<string>::format("Unknown", ctx);
  }
};

template <>
struct std::formatter<dcn::server::RouteArg> : std::formatter<std::string> {
  auto format(const dcn::server::RouteArg & arg, format_context& ctx) const {
    return formatter<string>::format(
      std::format("({}) [{}] {}", arg.getRequirement(), arg.getType(), arg.getData()), ctx);
  }
};