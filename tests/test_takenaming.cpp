// SPDX-License-Identifier: GPL-3.0-or-later
//
// Take filenames are user-facing and user-configurable, which makes them a
// string-handling problem with a security edge: the pattern is typed by hand,
// so it must not be able to write outside the recordings folder.
#include "application/TakeNaming.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_set>

using dave::application::expandTakeNamePattern;
using dave::application::kDefaultTakeNamePattern;
using dave::application::sanitizeTakeNameComponent;
using dave::application::TakeNameContext;
using dave::application::takeNamePatternIsUnique;
using dave::application::uniqueTakePath;

namespace {

TakeNameContext context(const char* track, int take) {
    TakeNameContext c;
    c.trackName = track;
    c.projectName = "My Session";
    c.date = "20260811";
    c.time = "143005";
    c.takeNumber = take;
    return c;
}

struct TempDir {
    TempDir() {
        path = std::filesystem::temp_directory_path() / "dave_takenaming_test";
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }
    ~TempDir() { std::filesystem::remove_all(path); }
    void touch(const std::string& name) {
        std::ofstream(path / name) << "x";
    }
    std::filesystem::path path;
};

} // namespace

TEST_CASE("the default pattern gives Trackname_01, _02, …", "[takenaming]") {
    CHECK(expandTakeNamePattern(kDefaultTakeNamePattern, context("Vocals", 1)) ==
          "Vocals_01");
    CHECK(expandTakeNamePattern(kDefaultTakeNamePattern, context("Vocals", 2)) ==
          "Vocals_02");
    CHECK(expandTakeNamePattern(kDefaultTakeNamePattern, context("Vocals", 9)) ==
          "Vocals_09");
    // Two digits keeps a file browser sorted; past 99 the number grows rather
    // than wrapping or truncating, because losing a take is not acceptable.
    CHECK(expandTakeNamePattern(kDefaultTakeNamePattern, context("Vocals", 10)) ==
          "Vocals_10");
    CHECK(expandTakeNamePattern(kDefaultTakeNamePattern, context("Vocals", 137)) ==
          "Vocals_137");
}

TEST_CASE("every token substitutes", "[takenaming]") {
    const auto c = context("Gtr DI", 3);
    CHECK(expandTakeNamePattern("{track}", c) == "Gtr_DI");
    CHECK(expandTakeNamePattern("{take}", c) == "03");
    CHECK(expandTakeNamePattern("{date}", c) == "20260811");
    CHECK(expandTakeNamePattern("{time}", c) == "143005");
    CHECK(expandTakeNamePattern("{project}", c) == "My_Session");
    CHECK(expandTakeNamePattern("{project}-{date}-{track}-{take}", c) ==
          "My_Session-20260811-Gtr_DI-03");
}

TEST_CASE("track names are reduced to filename-safe characters",
          "[takenaming]") {
    CHECK(sanitizeTakeNameComponent("Lead Vox") == "Lead_Vox");
    CHECK(sanitizeTakeNameComponent("Kick/Snare") == "KickSnare");
    CHECK(sanitizeTakeNameComponent("Bass: DI") == "Bass_DI");
    // Nothing usable left over still has to produce a name.
    CHECK(sanitizeTakeNameComponent("///") == "Track");
    CHECK(sanitizeTakeNameComponent("") == "Track");
}

TEST_CASE("a pattern cannot escape the recordings folder", "[takenaming]") {
    // The pattern is typed by the user. Path separators in it must not turn
    // into directories, and traversal must not reach outside.
    const auto c = context("Vox", 1);
    const std::string escaped =
        expandTakeNamePattern("../../etc/{track}", c);
    CHECK(escaped.find('/') == std::string::npos);
    CHECK(escaped.find("..") == std::string::npos);

    const std::string windows = expandTakeNamePattern("C:\\evil\\{track}", c);
    CHECK(windows.find('\\') == std::string::npos);
    CHECK(windows.find(':') == std::string::npos);
}

TEST_CASE("an unknown token stays visible rather than vanishing",
          "[takenaming]") {
    // Silently dropping {tack} would make a typo look like it worked.
    const auto out = expandTakeNamePattern("{track}_{tack}", context("Vox", 1));
    CHECK(out.find("tack") != std::string::npos);
}

TEST_CASE("an unterminated brace is treated as text", "[takenaming]") {
    const auto out = expandTakeNamePattern("{track}_{take", context("Vox", 1));
    CHECK(out.find("Vox") == 0);
    CHECK_FALSE(out.empty());
}

TEST_CASE("patterns are classified by whether they vary with the take number",
          "[takenaming]") {
    CHECK(takeNamePatternIsUnique(kDefaultTakeNamePattern));
    CHECK(takeNamePatternIsUnique("{take}"));
    CHECK_FALSE(takeNamePatternIsUnique("{track}"));
    CHECK_FALSE(takeNamePatternIsUnique("session"));
}

TEST_CASE("take numbers advance past files already on disk", "[takenaming]") {
    TempDir dir;
    dir.touch("Vocals_01.wav");
    dir.touch("Vocals_02.wav");

    std::unordered_set<std::string> reserved;
    const auto path = uniqueTakePath(dir.path, kDefaultTakeNamePattern,
                                     context("Vocals", 1), reserved);
    CHECK(path.filename().string() == "Vocals_03.wav");
}

TEST_CASE("tracks armed together never collide", "[takenaming]") {
    // Every file is opened before any exists on disk, so existence alone is
    // not enough — the reservation set is what keeps two armed tracks apart.
    TempDir dir;
    std::unordered_set<std::string> reserved;

    const auto a = uniqueTakePath(dir.path, kDefaultTakeNamePattern,
                                  context("Vox", 1), reserved);
    const auto b = uniqueTakePath(dir.path, kDefaultTakeNamePattern,
                                  context("Vox", 1), reserved);
    const auto c = uniqueTakePath(dir.path, kDefaultTakeNamePattern,
                                  context("Gtr", 1), reserved);

    CHECK(a.filename().string() == "Vox_01.wav");
    CHECK(b.filename().string() == "Vox_02.wav");   // same track, next take
    CHECK(c.filename().string() == "Gtr_01.wav");   // different track, own count
}

TEST_CASE("a pattern without a take token still terminates", "[takenaming]") {
    // Otherwise the search for a free name loops forever on one filename.
    TempDir dir;
    dir.touch("Vox.wav");
    std::unordered_set<std::string> reserved;

    const auto first = uniqueTakePath(dir.path, "{track}", context("Vox", 1),
                                      reserved);
    CHECK(first.filename().string() == "Vox_02.wav");

    const auto second = uniqueTakePath(dir.path, "{track}", context("Vox", 1),
                                       reserved);
    CHECK(second.filename().string() == "Vox_03.wav");
}

TEST_CASE("every produced name ends in .wav inside the given directory",
          "[takenaming]") {
    TempDir dir;
    std::unordered_set<std::string> reserved;
    for (const char* pattern :
         {kDefaultTakeNamePattern, "{track}", "{date}_{time}_{track}_{take}",
          "take{take}"}) {
        const auto path = uniqueTakePath(dir.path, pattern, context("Vox", 1),
                                         reserved);
        INFO("pattern " << pattern);
        CHECK(path.extension() == ".wav");
        CHECK(path.parent_path() == dir.path);
    }
}
