// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace dave::audio {

// Dave currently decodes an entire audio file into interleaved float samples
// and then deinterleaves it. RF64 takes are valid, but files beyond this
// encoded-size limit must wait for streaming playback rather than risking a
// very large pair of allocations. This is deliberately not a general memory
// budget: files below the limit can still require substantial decode memory.
inline constexpr std::uintmax_t kMaxInMemoryDecodeFileBytes =
    std::uintmax_t{4} * 1024 * 1024 * 1024;

[[nodiscard]] constexpr bool canDecodeFileInMemory(
    std::uintmax_t encodedFileBytes) noexcept {
    return encodedFileBytes <= kMaxInMemoryDecodeFileBytes;
}

} // namespace dave::audio
