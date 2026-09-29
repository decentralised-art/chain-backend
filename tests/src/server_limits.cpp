#include "unit-tests.hpp"

using namespace dcn;

TEST(ServerLimits, RefusesOversizedHeadersBeforeTheyComplete)
{
    EXPECT_FALSE(server::requestSizeRejection(512, false, 0, 1024).has_value());
    EXPECT_FALSE(server::requestSizeRejection(server::MAX_HEADER_SIZE, false, 0, 1024).has_value());
    EXPECT_EQ(
        server::requestSizeRejection(server::MAX_HEADER_SIZE + 1, false, 0, 1024),
        http::Code::RequestHeaderFieldsTooLarge);
}

TEST(ServerLimits, RefusesABodyLargerThanTheLimitFromItsDeclaredLength)
{
    EXPECT_FALSE(server::requestSizeRejection(200, true, 0, 1024).has_value());
    EXPECT_FALSE(server::requestSizeRejection(200, true, 1024, 1024).has_value());
    EXPECT_EQ(server::requestSizeRejection(200, true, 1025, 1024), http::Code::PayloadTooLarge);

    // The default admits a signed publication transaction carrying the largest initcode
    // (EIP-3860: 49152 bytes), hex-encoded in a JSON body, with room to spare.
    EXPECT_FALSE(server::requestSizeRejection(200, true, 4 * 49'152, server::DEFAULT_MAX_BODY_SIZE).has_value());
}
