// SPDX-License-Identifier: GPL-3.0-or-later
#include "document/ProjectFile.h"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>

namespace {

struct TempBundles {
    TempBundles() {
        root = std::filesystem::temp_directory_path() /
            ("dave_recording_bundle_" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
        original = root / "Original.dave";
        saveAs = root / "Copy.dave";
        std::filesystem::create_directories(original / "recordings");
    }
    ~TempBundles() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
    std::filesystem::path root;
    std::filesystem::path original;
    std::filesystem::path saveAs;
};

nlohmann::json projectJson(const std::filesystem::path& bundle) {
    std::ifstream input(bundle / "project.json");
    return nlohmann::json::parse(input);
}

} // namespace

TEST_CASE("recording assets stay in recordings and survive save load",
          "[recording][projectfile]") {
    TempBundles tmp;
    const auto wav = tmp.original / "recordings" /
                     "Dialogue_20260811-120000.wav";
    {
        std::ofstream output(wav, std::ios::binary);
        output << "finished wav bytes";
    }

    dave::document::Edit edit;
    const std::string trackId = edit.addTrack("Dialogue");
    dave::document::AudioAsset asset;
    asset.id = dave::document::AssetId{"take-sha"};
    asset.path = wav.string();
    asset.sampleRate = 48000;
    asset.channels = 1;
    asset.lengthSamples = 100;
    edit.loadAsset_(asset);
    dave::document::AudioClip clip;
    clip.asset = asset.id;
    clip.length = 100;
    edit.addClip(trackId, clip);

    REQUIRE(dave::document::saveBundle(tmp.original.string(), edit).ok);
    const auto saved = projectJson(tmp.original);
    REQUIRE(saved["assets"].size() == 1);
    CHECK(saved["assets"][0]["path"] ==
          "recordings/Dialogue_20260811-120000.wav");
    CHECK_FALSE(std::filesystem::exists(tmp.original / "assets" /
                                        "take-sha.wav"));

    dave::document::Edit loaded;
    REQUIRE(dave::document::loadBundle(tmp.original.string(), loaded).ok);
    REQUIRE(loaded.asset(asset.id) != nullptr);
    CHECK(std::filesystem::path(loaded.asset(asset.id)->path) == wav);
}

TEST_CASE("Save As copies a finished take into the new recordings folder",
          "[recording][projectfile]") {
    TempBundles tmp;
    const auto wav = tmp.original / "recordings" / "Boom.wav";
    {
        std::ofstream output(wav, std::ios::binary);
        output << "take";
    }

    dave::document::Edit edit;
    dave::document::AudioAsset asset;
    asset.id = dave::document::AssetId{"boom-sha"};
    asset.path = wav.string();
    asset.sampleRate = 48000;
    asset.channels = 2;
    asset.lengthSamples = 1;
    edit.loadAsset_(asset);

    std::filesystem::create_directories(tmp.saveAs / "recordings");
    {
        std::ofstream existing(tmp.saveAs / "recordings" / "Boom.wav",
                               std::ios::binary);
        existing << "different existing take";
    }

    REQUIRE(dave::document::saveBundle(tmp.saveAs.string(), edit).ok);
    CHECK(std::filesystem::exists(tmp.saveAs / "recordings" / "Boom_2.wav"));
    CHECK(projectJson(tmp.saveAs)["assets"][0]["path"] ==
          "recordings/Boom_2.wav");
}
