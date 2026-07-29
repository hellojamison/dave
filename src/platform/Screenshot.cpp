// SPDX-License-Identifier: GPL-3.0-or-later
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../../third_party/glfw/deps/stb_image_write.h"

#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#include <string>

namespace dave::platform {
namespace detail {

bool writePng(const std::string& path, int width, int height,
              const unsigned char* pixels, int strideBytes) {
    return stbi_write_png(path.c_str(), width, height, 4, pixels, strideBytes) != 0;
}

} // namespace detail
} // namespace dave::platform
