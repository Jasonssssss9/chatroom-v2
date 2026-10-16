#include "framing.h"

#include <arpa/inet.h>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <stdexcept>

using namespace chatroom;

TEST_CASE("Frames use a four-byte network-order length", "[framing]") {
    const auto frame = encode_frame("abc");
    REQUIRE(frame.size() == 7);
    CHECK(frame.substr(0, 4) == std::string("\0\0\0\3", 4));
    CHECK(frame.substr(4) == "abc");
}

TEST_CASE("Decoder retains partial headers and payloads", "[framing]") {
    FrameDecoder decoder;
    const auto frame = encode_frame("hello");
    decoder.append(frame.substr(0, 2));
    CHECK_FALSE(decoder.next().has_value());
    decoder.append(frame.substr(2, 4));
    CHECK_FALSE(decoder.next().has_value());
    decoder.append(frame.substr(6));
    REQUIRE(decoder.next() == "hello");
    CHECK_FALSE(decoder.next().has_value());
}

TEST_CASE("Decoder preserves consecutive frames and trailing partial data", "[framing]") {
    FrameDecoder decoder;
    const auto third = encode_frame("third");
    decoder.append(encode_frame("first") + encode_frame("second") + third.substr(0, 5));
    REQUIRE(decoder.next() == "first");
    REQUIRE(decoder.next() == "second");
    CHECK_FALSE(decoder.next().has_value());
    decoder.append(third.substr(5));
    REQUIRE(decoder.next() == "third");
}

TEST_CASE("Frames preserve UTF-8 and embedded zero bytes", "[framing]") {
    const std::string utf8 = u8"\u4f60\u597d\U0001f600";
    const std::string payload = utf8 + std::string("\0end", 4);
    CHECK(utf8.size() == 10);
    FrameDecoder decoder;
    decoder.append(encode_frame(payload));
    REQUIRE(decoder.next() == payload);
}

TEST_CASE("Framing supports empty payloads and the maximum payload size", "[framing]") {
    FrameDecoder decoder;
    const std::string payload(kMaxFrameSize, 'x');
    decoder.append(encode_frame("") + encode_frame(payload));
    REQUIRE(decoder.next() == "");
    REQUIRE(decoder.next() == payload);
    CHECK_FALSE(decoder.next().has_value());
}

TEST_CASE("Oversized frames are rejected before their payload arrives", "[framing]") {
    CHECK_THROWS_AS(encode_frame(std::string(kMaxFrameSize + 1, 'x')), std::length_error);
    const auto length = htonl(static_cast<std::uint32_t>(kMaxFrameSize + 1));
    FrameDecoder decoder;
    decoder.append(std::string_view(reinterpret_cast<const char*>(&length), sizeof(length)));
    CHECK_THROWS_AS(decoder.next(), std::length_error);
}
