// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>
#include <string>
#include <unordered_set>

namespace dave::application {

// How recorded take files are named.
//
// The pattern is a user preference, so it lives here rather than inside
// DaveApp: that file is not in the test target, and filename generation is
// exactly the kind of string work that wants tests.
//
// Tokens, all optional:
//   {track}    the armed track's name, reduced to filename-safe characters
//   {take}     the take number, zero-padded to two digits (01, 02, … 100)
//   {date}     the take's start date, YYYYMMDD
//   {time}     the take's start time, HHMMSS
//   {project}  the project's name, without the .dave extension
//
// Anything else is copied through verbatim, including an unrecognised
// {token} — silently dropping it would make a typo look like it worked.
inline constexpr const char* kDefaultTakeNamePattern = "{track}_{take}";

struct TakeNameContext {
    std::string trackName;
    std::string projectName;
    std::string date;        // YYYYMMDD
    std::string time;        // HHMMSS
    int takeNumber = 1;
};

// Reduce a string to characters that are safe in a filename on every platform
// we target. Spaces become underscores; anything else non-alphanumeric is
// dropped. Never returns empty — an unnameable input falls back to `fallback`.
std::string sanitizeTakeNameComponent(const std::string& name,
                                      const std::string& fallback = "Track");

// Substitute the tokens. The result is sanitized as a whole, so a pattern
// containing a path separator cannot escape the recordings folder.
std::string expandTakeNamePattern(const std::string& pattern,
                                  const TakeNameContext& context);

// True when the pattern varies with the take number. A pattern without {take}
// collides with itself on the second take of a session, so callers need to
// know to disambiguate.
bool takeNamePatternIsUnique(const std::string& pattern);

// The first free path in `directory`, trying take numbers upward from
// `context.takeNumber`. `reserved` carries paths claimed earlier in the same
// arm — several tracks are opened before any file exists on disk, so
// existence alone is not enough.
//
// A pattern without {take} still terminates: it gets a numeric suffix once the
// unnumbered name is taken.
std::filesystem::path uniqueTakePath(const std::filesystem::path& directory,
                                     const std::string& pattern,
                                     const TakeNameContext& context,
                                     std::unordered_set<std::string>& reserved);

} // namespace dave::application
