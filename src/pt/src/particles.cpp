#include "particles.hpp"
#include "chain.hpp"

namespace dcn::parse
{
    template<>
    Result<json> parseToJson(std::vector<Particles> particles, use_json_t) 
    {
        json arr = json::array();

        for (const auto& particle : particles) {
            json obj;
            obj["path"] = particle.path();
            obj["data"] = particle.data();
            arr.push_back(obj);
        }

        return arr;
    }

    template<>
    Result<std::vector<Particles>> parseFromJson(json json_val, use_json_t) 
    {
        if (!json_val.is_array())
            return std::unexpected(ParseError{ParseError::Kind::TYPE_MISMATCH, "Invalid JSON object"});

        std::vector<Particles> result;

        for (const auto& item : json_val) {
            if (!item.contains("path") || !item.contains("data"))
                return std::unexpected(ParseError{ParseError::Kind::INVALID_VALUE, "path or data not found"});

            Particles particles;
            particles.set_path(item["path"].get<std::string>());

            for (const auto& val : item["data"]) {
                particles.add_data(val.get<std::uint32_t>());
            }

            result.push_back(std::move(particles));
        }

        return result;
    }

    template<>
    Result<std::vector<Particles>> decodeBytes(const std::vector<std::uint8_t>& bytes)
    {
        // Runner output can come from a remote node, so every offset and length is
        // checked against the buffer before it is followed.
        const auto malformed = []()
        {
            return std::unexpected(ParseError{ParseError::Kind::INVALID_VALUE, "malformed Particles[] encoding"});
        };
        const std::uint8_t * data = bytes.data();
        const std::size_t size = bytes.size();

        const auto array_offset = chain::readWordAsSizeT(data, size, 0);
        if(!array_offset) return malformed();
        const auto count = chain::readWordAsSizeT(data, size, *array_offset);
        if(!count) return malformed();

        // Element offsets are relative to the first head word; each element needs one.
        const std::size_t heads = *array_offset + 32;
        if(*count > (size - heads) / 32) return malformed();

        std::vector<Particles> result;
        result.reserve(*count);
        for(std::size_t i = 0; i < *count; ++i)
        {
            const auto element_offset = chain::readWordAsSizeT(data, size, heads + i * 32);
            if(!element_offset || *element_offset > size - heads) return malformed();
            const std::uint8_t * element = data + heads + *element_offset;
            const std::size_t element_size = size - heads - *element_offset;

            const auto path_offset = chain::readWordAsSizeT(element, element_size, 0);
            const auto values_offset = chain::readWordAsSizeT(element, element_size, 32);
            if(!path_offset || !values_offset) return malformed();

            const auto path = chain::decodeAbiString(element, element_size, *path_offset);
            const auto values = chain::decodeAbiUint32Array(element, element_size, *values_offset);
            if(!path || !values) return malformed();

            Particles particles;
            particles.set_path(*path);
            for(const std::uint32_t value : *values)
            {
                particles.add_data(value);
            }
            result.push_back(std::move(particles));
        }

        return result;
    }
}
