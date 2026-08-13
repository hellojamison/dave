// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio/AudioImportPolicy.h"
#include "document/Edit.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

using dave::audio::canDecodeFileInMemory;
using dave::audio::kMaxInMemoryDecodeFileBytes;

namespace {

struct TempFile {
    std::filesystem::path path;
    ~TempFile() {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }
};

TempFile uniqueTempFile() {
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    return {std::filesystem::temp_directory_path() /
            ("dave_oversized_import_" + std::to_string(suffix) + ".wav")};
}

} // namespace

TEST_CASE("the in-memory audio import limit has an exact 4 GiB boundary",
          "[audioimport]") {
    CHECK(canDecodeFileInMemory(kMaxInMemoryDecodeFileBytes - 1));
    CHECK(canDecodeFileInMemory(kMaxInMemoryDecodeFileBytes));
    CHECK_FALSE(canDecodeFileInMemory(kMaxInMemoryDecodeFileBytes + 1));
}

TEST_CASE("an oversized sparse file is refused before central audio import",
          "[audioimport]") {
    auto tmp = uniqueTempFile();
    {
        // A valid empty PCM WAV makes this a causal test of the size guard. If
        // the guard is removed, dr_wav accepts the header and import succeeds
        // after the (undesired) multi-gigabyte hash instead of failing merely
        // because the sparse test file is malformed.
        constexpr std::array<std::uint8_t, 44> kEmptyMonoWav = {
            'R', 'I', 'F', 'F', 36, 0, 0, 0,
            'W', 'A', 'V', 'E', 'f', 'm', 't', ' ',
            16, 0, 0, 0, 1, 0, 1, 0,
            0x80, 0xBB, 0, 0, 0, 0x77, 1, 0,
            2, 0, 16, 0, 'd', 'a', 't', 'a',
            0, 0, 0, 0,
        };
        std::ofstream create(tmp.path, std::ios::binary);
        REQUIRE(create.good());
        create.write(reinterpret_cast<const char*>(kEmptyMonoWav.data()),
                     static_cast<std::streamsize>(kEmptyMonoWav.size()));
        REQUIRE(create.good());
    }

    std::error_code resizeError;
    std::filesystem::resize_file(
        tmp.path, kMaxInMemoryDecodeFileBytes + 1, resizeError);
    REQUIRE_FALSE(resizeError);
    REQUIRE(std::filesystem::file_size(tmp.path) ==
            kMaxInMemoryDecodeFileBytes + 1);

    dave::document::Edit edit;
    CHECK_FALSE(edit.importAsset(tmp.path.string()).valid());
}
