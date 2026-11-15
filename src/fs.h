// std::filesystem is C++17; ghc::filesystem provides the same API for C++11,
// including wide-character paths on Windows. Everything in the launcher goes
// through this alias, so moving to std::filesystem later is a one-line change.
#ifndef IGWL_FS_H
#define IGWL_FS_H

#include <ghc/filesystem.hpp>

namespace fs = ghc::filesystem;

#endif
