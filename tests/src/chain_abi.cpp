#include "unit-tests.hpp"

#include "fake_eth_node.hpp"

#include <algorithm>
#include <limits>
#include <string>
#include <vector>

using namespace dcn;
using namespace dcn::tests;
using namespace dcn::tests::fake_eth;

namespace
{
    std::vector<std::uint8_t> bytesOf(const std::string & hex)
    {
        const auto bytes = evmc::from_hex(hex).value();
        return std::vector<std::uint8_t>(bytes.begin(), bytes.end());
    }

    std::vector<std::uint8_t> word(const std::uint64_t value)
    {
        return pt::abi::uintComponent(value).data;
    }
}

TEST_F(UnitTest, Chain_AbiReaders_RefuseOffsetsThatWouldWrapAround)
{
    const std::vector<std::uint8_t> data(64, 0);
    const std::size_t huge = std::numeric_limits<std::size_t>::max() - 8;

    EXPECT_FALSE(chain::readWordAsSizeT(data.data(), data.size(), huge).has_value());
    EXPECT_FALSE(chain::readWordAsSizeT(data.data(), data.size(), 33).has_value());
    EXPECT_TRUE(chain::readWordAsSizeT(data.data(), data.size(), 32).has_value());

    EXPECT_FALSE(chain::decodeAbiString(data.data(), data.size(), huge).has_value());
    EXPECT_FALSE(chain::decodeAbiUint32Array(data.data(), data.size(), huge).has_value());

    // A string array whose element offset points past the buffer.
    std::vector<std::uint8_t> array = word(1);
    const auto distant = word(std::numeric_limits<std::uint64_t>::max() - 16);
    array.insert(array.end(), distant.begin(), distant.end());
    EXPECT_FALSE(chain::decodeAbiStringArray(array.data(), array.size(), 0).has_value());
}

TEST_F(UnitTest, Pt_DecodeParticles_RoundTripsRunnerOutput)
{
    const auto bytes = bytesOf(encodeParticlesResult({
        {"/Root:0", {1, 2, 3}},
        {"/Root:1/Child:0", {}},
        {"/Root:2", {4294967295u}}}));

    const auto particles = parse::decodeBytes<std::vector<Particles>>(bytes);
    ASSERT_TRUE(particles.has_value());
    ASSERT_EQ(particles->size(), 3u);
    EXPECT_EQ(particles->at(0).path(), "/Root:0");
    EXPECT_EQ(particles->at(0).data_size(), 3);
    EXPECT_EQ(particles->at(0).data(2), 3u);
    EXPECT_EQ(particles->at(1).path(), "/Root:1/Child:0");
    EXPECT_EQ(particles->at(1).data_size(), 0);
    EXPECT_EQ(particles->at(2).data(0), 4294967295u);
}

TEST_F(UnitTest, Pt_DecodeParticles_RefusesMalformedRunnerOutputWithoutReadingPastIt)
{
    const auto valid = bytesOf(encodeParticlesResult({{"/Root:0", {1, 2, 3}}}));

    // Every truncation of a valid encoding is refused.
    for(std::size_t size = 0; size < valid.size(); size += 7)
    {
        const std::vector<std::uint8_t> truncated(valid.begin(), valid.begin() + static_cast<std::ptrdiff_t>(size));
        EXPECT_FALSE(parse::decodeBytes<std::vector<Particles>>(truncated).has_value()) << size;
    }

    // A count far larger than the buffer could hold.
    std::vector<std::uint8_t> huge_count = word(32);
    const auto count = word(1ull << 40);
    huge_count.insert(huge_count.end(), count.begin(), count.end());
    EXPECT_FALSE(parse::decodeBytes<std::vector<Particles>>(huge_count).has_value());

    // An element offset pointing outside the buffer.
    std::vector<std::uint8_t> bad_element = word(32);
    for(const auto & part : {word(1), word(std::numeric_limits<std::uint64_t>::max() - 64)})
    {
        bad_element.insert(bad_element.end(), part.begin(), part.end());
    }
    EXPECT_FALSE(parse::decodeBytes<std::vector<Particles>>(bad_element).has_value());

    // A value that does not fit the uint32 the runner returns.
    auto oversized = bytesOf(encodeParticlesResult({{"/Root:0", {1}}}));
    oversized[oversized.size() - 5] = 0x01;
    EXPECT_FALSE(parse::decodeBytes<std::vector<Particles>>(oversized).has_value());
}

TEST_F(UnitTest, Chain_ReadUint64Word_RejectsOverflowAndShortInput)
{
    std::vector<std::uint8_t> word(32, 0);
    word[31] = 0x2A;
    EXPECT_EQ(chain::readUint64Word(word), std::optional<std::uint64_t>(42));

    std::fill(word.begin() + 24, word.end(), 0xFF);
    EXPECT_EQ(chain::readUint64Word(word), std::optional<std::uint64_t>(std::numeric_limits<std::uint64_t>::max()));

    word[23] = 0x01;
    EXPECT_FALSE(chain::readUint64Word(word).has_value());

    EXPECT_FALSE(chain::readUint64Word(std::vector<std::uint8_t>(31, 0)).has_value());
    EXPECT_FALSE(chain::readUint64Word(std::vector<std::uint8_t>(32, 0), 1).has_value());
    EXPECT_FALSE(chain::readUint64Word(word.data(), word.size(), std::numeric_limits<std::size_t>::max() - 8).has_value());
}

TEST_F(UnitTest, Chain_ReadAddressWord_RejectsWrappingOffset)
{
    const std::vector<std::uint8_t> word(32, 0);
    EXPECT_TRUE(chain::readAddressWord(word).has_value());
    EXPECT_FALSE(chain::readAddressWord(word.data(), word.size(), std::numeric_limits<std::size_t>::max() - 8).has_value());
}

TEST_F(UnitTest, Chain_ToHexQuantity_EncodesSignedAndUnsigned)
{
    EXPECT_EQ(chain::toHexQuantity(0), "0x0");
    EXPECT_EQ(chain::toHexQuantity(std::int64_t{-5}), "0x0");
    EXPECT_EQ(chain::toHexQuantity(std::int64_t{255}), "0xff");
    EXPECT_EQ(chain::toHexQuantity(std::numeric_limits<std::uint64_t>::max()), "0xffffffffffffffff");
}

TEST_F(UnitTest, Chain_IsBytes32Hex_RequiresExactly32Bytes)
{
    const std::string hash(64, 'a');
    EXPECT_TRUE(chain::isBytes32Hex(hash));
    EXPECT_TRUE(chain::isBytes32Hex("0x" + hash));
    EXPECT_FALSE(chain::isBytes32Hex("0x" + hash.substr(2)));
    EXPECT_FALSE(chain::isBytes32Hex("0x" + std::string(63, 'a') + "g"));
}

TEST_F(UnitTest, Chain_KeccakBytes_SpanMatchesPointerForm)
{
    const std::vector<std::uint8_t> bytes{1, 2, 3};
    EXPECT_EQ(chain::keccakBytes(bytes), chain::keccakBytes(bytes.data(), bytes.size()));
}
