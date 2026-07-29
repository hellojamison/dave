// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dave::document {

// Minimal SHA-256 (public domain, Brad Conte's implementation). Used to
// content-address audio assets — same file content → same id, so double-import
// dedupes and edits reference stable ids.
//
// We avoid OpenSSL to keep the build dependency-free and licensing-clean.
std::vector<uint8_t> sha256(const uint8_t* data, size_t len);

// Hex string of SHA-256 over a file's bytes. Empty string on read error.
std::string sha256HexOfFile(const std::string& filePath);

} // namespace dave::document
