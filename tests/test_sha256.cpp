// SPDX-License-Identifier: GPL-3.0-or-later
#include "document/Sha256.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

using dave::document::sha256;

namespace {

std::string hex(const std::vector<uint8_t>& bytes) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (uint8_t b : bytes) {
        out.push_back(digits[b >> 4]);
        out.push_back(digits[b & 0x0f]);
    }
    return out;
}

std::string hashOf(const std::string& s) {
    return hex(sha256(reinterpret_cast<const uint8_t*>(s.data()), s.size()));
}

} // namespace

// Asset ids are content addresses, so a wrong digest silently breaks dedupe
// and makes saved projects reference assets that can't be found again. These
// are the published NIST vectors — checking against a second implementation of
// our own would only prove the two agree with each other.
TEST_CASE("sha256 matches published vectors", "[sha256]") {
    CHECK(hashOf("") ==
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(hashOf("abc") ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(hashOf("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

// The 55/56/64-byte boundaries are where SHA-256's padding block splits; an
// off-by-one in the padding logic passes short inputs and fails exactly here.
TEST_CASE("sha256 handles padding boundaries", "[sha256]") {
    CHECK(hashOf(std::string(55, 'a')) ==
          "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318");
    CHECK(hashOf(std::string(56, 'a')) ==
          "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a");
    CHECK(hashOf(std::string(64, 'a')) ==
          "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb");
}

TEST_CASE("sha256 is sensitive to single-bit changes", "[sha256]") {
    CHECK(hashOf("dave") != hashOf("Dave"));
    CHECK(hashOf("a") != hashOf("b"));
}
