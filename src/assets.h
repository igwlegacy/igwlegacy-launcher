// Files from assets/ compiled into the executable (see cmake/EmbedFile.cmake
// and the igwl_embed() calls in CMakeLists.txt).
#ifndef IGWL_ASSETS_H
#define IGWL_ASSETS_H

#include <cstddef>

namespace igwl {
namespace assets {

// assets/logo_flat.png, shown at the left of the launcher window.
extern const unsigned char logo_flat_png[];
extern const std::size_t logo_flat_png_size;

}  // namespace assets
}  // namespace igwl

#endif
