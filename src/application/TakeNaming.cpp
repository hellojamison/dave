// SPDX-License-Identifier: GPL-3.0-or-later
#include "application/TakeNaming.h"

#include <cctype>
#include <cstdio>

namespace dave::application {

namespace {

// Two digits is the DAW convention (Vocals_01) and stays sorted in a file
// browser up to 99. Past that the number simply grows rather than wrapping or
// truncating — a hundred takes is unusual, losing one is not acceptable.
std::string formatTakeNumber(int takeNumber) {
    if (takeNumber < 0) takeNumber = 0;
    char buffer[16]{};
    std::snprintf(buffer, sizeof(buffer), "%02d", takeNumber);
    return buffer;
}

} // namespace

std::string sanitizeTakeNameComponent(const std::string& name,
                                      const std::string& fallback) {
    std::string safe;
    safe.reserve(name.size());
    for (unsigned char c : name) {
        if (std::isalnum(c) || c == '-' || c == '_') {
            safe.push_back(static_cast<char>(c));
        } else if (std::isspace(c)) {
            safe.push_back('_');
        }
    }
    return safe.empty() ? fallback : safe;
}

std::string expandTakeNamePattern(const std::string& pattern,
                                  const TakeNameContext& context) {
    std::string out;
    out.reserve(pattern.size() + 16);

    for (std::size_t i = 0; i < pattern.size();) {
        if (pattern[i] != '{') {
            out.push_back(pattern[i++]);
            continue;
        }
        const std::size_t close = pattern.find('}', i + 1);
        if (close == std::string::npos) {
            // An unterminated brace is a typo, not a token. Copy the rest
            // through so the user sees it in the preview.
            out.append(pattern, i, std::string::npos);
            break;
        }
        const std::string token = pattern.substr(i + 1, close - i - 1);
        if (token == "track") {
            out += sanitizeTakeNameComponent(context.trackName);
        } else if (token == "take") {
            out += formatTakeNumber(context.takeNumber);
        } else if (token == "date") {
            out += context.date;
        } else if (token == "time") {
            out += context.time;
        } else if (token == "project") {
            out += sanitizeTakeNameComponent(context.projectName, "Session");
        } else {
            // Unknown token: keep it visible rather than silently vanishing.
            out.append(pattern, i, close - i + 1);
        }
        i = close + 1;
    }

    // Sanitize the whole result, not just the substituted parts: the pattern
    // itself is user input, and a "/" in it would otherwise write outside the
    // recordings folder.
    return sanitizeTakeNameComponent(out);
}

bool takeNamePatternIsUnique(const std::string& pattern) {
    TakeNameContext a;
    TakeNameContext b;
    a.takeNumber = 1;
    b.takeNumber = 2;
    return expandTakeNamePattern(pattern, a) != expandTakeNamePattern(pattern, b);
}

std::filesystem::path uniqueTakePath(
    const std::filesystem::path& directory, const std::string& pattern,
    const TakeNameContext& context,
    std::unordered_set<std::string>& reserved) {
    const bool numbered = takeNamePatternIsUnique(pattern);

    TakeNameContext probe = context;
    if (probe.takeNumber < 1) probe.takeNumber = 1;

    for (int attempt = 0;; ++attempt) {
        std::string stem;
        if (numbered) {
            probe.takeNumber = context.takeNumber + attempt;
            stem = expandTakeNamePattern(pattern, probe);
        } else {
            // The pattern ignores {take}, so disambiguate with a suffix rather
            // than looping forever on the same name.
            stem = expandTakeNamePattern(pattern, probe);
            if (attempt > 0) stem += "_" + formatTakeNumber(attempt + 1);
        }

        const auto candidate = directory / (stem + ".wav");
        const std::string key = candidate.lexically_normal().string();
        std::error_code exists;
        if (!std::filesystem::exists(candidate, exists) &&
            reserved.insert(key).second) {
            return candidate;
        }
    }
}

} // namespace dave::application
