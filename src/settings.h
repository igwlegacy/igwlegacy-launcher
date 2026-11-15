// User settings stored next to the launcher binary, currently only the
// server to sync from. They override the compiled-in defaults.
#ifndef IGWL_SETTINGS_H
#define IGWL_SETTINGS_H

#include "fs.h"

#include <string>

namespace igwl {

struct Settings {
    std::string base_url;  // empty: use the compiled-in IGWL_BASE_URL
};

// <directory of the launcher>/igwlegacy-launcher-config.json
fs::path settings_path(const fs::path& launcher);

// Reads `path` into `settings`. A missing file is not an error and leaves
// the defaults; an unreadable or malformed one returns false with `error`
// set, so the caller can warn and carry on with the defaults.
bool load_settings(const fs::path& path, Settings& settings, std::string& error);

// Writes `settings` to `path`, keeping any other keys already in the file.
// Writes a temporary file and renames it, so a crash never leaves a
// truncated config behind.
bool save_settings(const fs::path& path, const Settings& settings, std::string& error);

// Trims `url` and checks that it is an http:// or https:// URL with a host;
// appends the trailing '/' the server layout needs. Returns false with
// `error` set otherwise.
bool normalize_base_url(std::string& url, std::string& error);

}  // namespace igwl

#endif
