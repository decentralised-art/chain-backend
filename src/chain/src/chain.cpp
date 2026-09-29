#include "chain.hpp"

#include <algorithm>

#include "crypto.hpp"

namespace dcn::chain
{
    std::vector<std::uint8_t> constructSelector(std::string signature)
    {
        std::uint8_t hash[32];
        crypto::Keccak256::getHash(reinterpret_cast<const uint8_t*>(signature.data()), signature.size(), hash);
        return std::vector<std::uint8_t>(hash, hash + 4);
    }


    evmc::bytes32 constructEventTopic(std::string signature)
    {
        evmc::bytes32 topic{};
        crypto::Keccak256::getHash(reinterpret_cast<const uint8_t*>(signature.data()), signature.size(), topic.bytes);
        return topic;
    }

    std::optional<std::vector<evmc::bytes32>> decodeTopicWords(const std::vector<std::string> & topics_hex)
    {
        if(topics_hex.empty())
        {
            return std::nullopt;
        }

        std::vector<evmc::bytes32> topic_words;
        topic_words.reserve(topics_hex.size());
        for(const std::string & topic_hex : topics_hex)
        {
            const auto topic_word = evmc::from_hex<evmc::bytes32>(topic_hex);
            if(!topic_word)
            {
                return std::nullopt;
            }
            topic_words.push_back(*topic_word);
        }

        return topic_words;
    }

    std::uint64_t readUint256(const std::vector<std::uint8_t> & bytes, std::size_t offset) {
        std::uint64_t value = 0;
        for (std::size_t i = 0; i < 32; ++i) {
            value <<= 8;
            value |= bytes[offset + i];
        }
        return value;
    }


    std::optional<std::size_t> readWordAsSizeT(const std::uint8_t* data, std::size_t data_size, std::size_t offset)
    {
        // Offsets come from untrusted ABI data; `offset + 32` could wrap around.
        if(data == nullptr || offset > data_size || data_size - offset < 32)
        {
            return std::nullopt;
        }

        std::size_t value = 0;

        constexpr std::size_t prefix = 32 - sizeof(std::size_t);
        for(std::size_t i = 0; i < prefix; ++i)
        {
            if(data[offset + i] != 0)
            {
                return std::nullopt;
            }
        }

        for(std::size_t i = prefix; i < 32; ++i)
        {
            value = (value << 8) | data[offset + i];
        }

        return value;
    }

    std::optional<std::uint32_t> readUint32Word(const std::uint8_t* data, std::size_t data_size, std::size_t offset)
    {
        const auto value_res = readWordAsSizeT(data, data_size, offset);
        if(!value_res || *value_res > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()))
        {
            return std::nullopt;
        }

        return static_cast<std::uint32_t>(*value_res);
    }

    std::optional<std::string> decodeAbiString(const std::uint8_t* data, std::size_t data_size, std::size_t string_offset)
    {
        const auto length_res = readWordAsSizeT(data, data_size, string_offset);
        if(!length_res)
        {
            return std::nullopt;
        }

        const std::size_t length = *length_res;
        if(string_offset + 32 > data_size || length > (data_size - (string_offset + 32)))
        {
            return std::nullopt;
        }

        return std::string(reinterpret_cast<const char*>(data + string_offset + 32), length);
    }


    std::optional<std::vector<std::string>> decodeAbiStringArray(const std::uint8_t* data, std::size_t data_size, std::size_t array_offset)
    {
        const auto length_res = readWordAsSizeT(data, data_size, array_offset);
        if(!length_res)
        {
            return std::nullopt;
        }

        const std::size_t length = *length_res;
        const std::size_t head_start = array_offset + 32;

        if(head_start > data_size)
        {
            return std::nullopt;
        }

        if(length > (std::numeric_limits<std::size_t>::max() / 32))
        {
            return std::nullopt;
        }

        const std::size_t head_size = length * 32;
        if(head_size > (data_size - head_start))
        {
            return std::nullopt;
        }

        std::vector<std::string> out;
        out.reserve(length);

        for(std::size_t i = 0; i < length; ++i)
        {
            const auto rel_offset_res = readWordAsSizeT(data, data_size, head_start + i * 32);
            if(!rel_offset_res || *rel_offset_res > data_size - head_start)
            {
                return std::nullopt;
            }

            const std::size_t elem_offset = head_start + *rel_offset_res;
            const auto elem_res = decodeAbiString(data, data_size, elem_offset);
            if(!elem_res)
            {
                return std::nullopt;
            }

            out.push_back(*elem_res);
        }

        return out;
    }

    std::optional<std::vector<std::int32_t>> decodeAbiInt32Array(const std::uint8_t* data, std::size_t data_size, std::size_t array_offset)
    {
        const auto length_res = readWordAsSizeT(data, data_size, array_offset);
        if(!length_res)
        {
            return std::nullopt;
        }

        const std::size_t length = *length_res;
        const std::size_t first_value_offset = array_offset + 32;

        if(first_value_offset > data_size)
        {
            return std::nullopt;
        }

        if(length > (std::numeric_limits<std::size_t>::max() / 32))
        {
            return std::nullopt;
        }

        const std::size_t values_size = length * 32;
        if(values_size > (data_size - first_value_offset))
        {
            return std::nullopt;
        }

        std::vector<std::int32_t> out;
        out.reserve(length);

        for(std::size_t i = 0; i < length; ++i)
        {
            const std::size_t word_offset = first_value_offset + i * 32;
            const std::uint8_t* word = data + word_offset;

            const std::uint32_t raw_value =
                (static_cast<std::uint32_t>(word[28]) << 24) |
                (static_cast<std::uint32_t>(word[29]) << 16) |
                (static_cast<std::uint32_t>(word[30]) << 8) |
                static_cast<std::uint32_t>(word[31]);

            const bool negative = (raw_value & 0x80000000u) != 0;
            const std::uint8_t expected_sign = negative ? 0xFF : 0x00;
            for(std::size_t b = 0; b < 28; ++b)
            {
                if(word[b] != expected_sign)
                {
                    return std::nullopt;
                }
            }

            out.push_back(static_cast<std::int32_t>(raw_value));
        }

        return out;
    }

    std::optional<std::vector<std::uint32_t>> decodeAbiUint32Array(const std::uint8_t* data, std::size_t data_size, std::size_t array_offset)
    {
        const auto length_res = readWordAsSizeT(data, data_size, array_offset);
        if(!length_res)
        {
            return std::nullopt;
        }

        const std::size_t length = *length_res;
        const std::size_t first_value_offset = array_offset + 32;

        if(first_value_offset > data_size)
        {
            return std::nullopt;
        }

        if(length > (std::numeric_limits<std::size_t>::max() / 32))
        {
            return std::nullopt;
        }

        const std::size_t values_size = length * 32;
        if(values_size > (data_size - first_value_offset))
        {
            return std::nullopt;
        }

        std::vector<std::uint32_t> out;
        out.reserve(length);

        for(std::size_t i = 0; i < length; ++i)
        {
            const std::size_t word_offset = first_value_offset + i * 32;
            const std::uint8_t* word = data + word_offset;

            for(std::size_t b = 0; b < 28; ++b)
            {
                if(word[b] != 0)
                {
                    return std::nullopt;
                }
            }

            const std::uint32_t raw_value =
                (static_cast<std::uint32_t>(word[28]) << 24) |
                (static_cast<std::uint32_t>(word[29]) << 16) |
                (static_cast<std::uint32_t>(word[30]) << 8) |
                static_cast<std::uint32_t>(word[31]);

            out.push_back(raw_value);
        }

        return out;
    }

    std::optional<std::string> decodeRevertReason(const std::span<const std::uint8_t> revert_data)
    {
        static const std::vector<std::uint8_t> ERROR_SELECTOR = constructSelector("Error(string)");
        if(revert_data.size() < ERROR_SELECTOR.size()
           || !std::equal(ERROR_SELECTOR.begin(), ERROR_SELECTOR.end(), revert_data.begin()))
        {
            return std::nullopt;
        }

        const std::uint8_t * payload = revert_data.data() + ERROR_SELECTOR.size();
        const std::size_t payload_size = revert_data.size() - ERROR_SELECTOR.size();
        const auto reason_offset = readWordAsSizeT(payload, payload_size, 0);
        if(!reason_offset) return std::nullopt;
        return decodeAbiString(payload, payload_size, *reason_offset);
    }
}
