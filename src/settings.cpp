#include "settings.h"

#include <nlohmann/json.hpp>

#include <cctype>
#include <iterator>
#include <system_error>

namespace igwl {

namespace {

const char kSettingsFile[] = "igwlegacy-launcher-config.json";

// Settings files are tiny; anything bigger is not ours.
const std::uintmax_t kMaxSettingsSize = 64 * 1024;

bool starts_with_ignoring_case(const std::string& text, const std::string& prefix) {
    if (text.size() < prefix.size()) {
        return false;
    }
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(text[i])) != prefix[i]) {
            return false;
        }
    }
    return true;
}

// Reads the file as a JSON object. `exists` tells a missing file apart from
// a broken one.
bool read_object(const fs::path& path, nlohmann::json& object, bool& exists, std::string& error) {
    std::error_code code;
    exists = fs::exists(path, code);
    if (!exists) {
        object = nlohmann::json::object();
        return true;
    }
    if (fs::file_size(path, code) > kMaxSettingsSize || code) {
        error = path.u8string() + " is unreadable or too large";
        return false;
    }
    fs::ifstream input(path, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (!input && !input.eof()) {
        error = "could not read " + path.u8string();
        return false;
    }
    object = nlohmann::json::parse(text, nullptr, false);
    if (object.is_discarded() || !object.is_object()) {
        error = path.u8string() + " is not a JSON object";
        return false;
    }
    return true;
}

}  // namespace

fs::path settings_path(const fs::path& launcher) {
    return launcher.parent_path() / kSettingsFile;
}

bool load_settings(const fs::path& path, Settings& settings, std::string& error) {
    nlohmann::json object;
    bool exists = false;
    if (!read_object(path, object, exists, error)) {
        return false;
    }
    if (!exists) {
        return true;
    }
    const nlohmann::json::const_iterator url = object.find("base_url");
    if (url != object.end()) {
        if (!url->is_string()) {
            error = path.u8string() + ": base_url must be a string";
            return false;
        }
        std::string value = url->get<std::string>();
        // An empty value means "the default", like a missing one.
        if (!value.empty()) {
            std::string reason;
            if (!normalize_base_url(value, reason)) {
                error = path.u8string() + ": " + reason;
                return false;
            }
        }
        settings.base_url = value;
    }
    return true;
}

bool save_settings(const fs::path& path, const Settings& settings, std::string& error) {
    nlohmann::json object;
    bool exists = false;
    std::string ignored;
    // A broken file gets replaced rather than blocking the save forever.
    if (!read_object(path, object, exists, ignored)) {
        object = nlohmann::json::object();
    }
    if (settings.base_url.empty()) {
        // Back to the default; future builds with another default follow it.
        object.erase("base_url");
    } else {
        object["base_url"] = settings.base_url;
    }

    fs::path temporary = path;
    temporary += ".tmp";
    {
        fs::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output << object.dump(2) << '\n';
        output.close();
        if (!output) {
            std::error_code ignored_code;
            fs::remove(temporary, ignored_code);
            error = "could not write " + temporary.u8string();
            return false;
        }
    }
    std::error_code code;
    fs::rename(temporary, path, code);
    if (code) {
        fs::remove(temporary, code);
        error = "could not replace " + path.u8string();
        return false;
    }
    return true;
}

bool normalize_base_url(std::string& url, std::string& error) {
    const std::string whitespace = " \t\r\n";
    const std::size_t first = url.find_first_not_of(whitespace);
    if (first == std::string::npos) {
        error = "the server URL is empty";
        return false;
    }
    url = url.substr(first, url.find_last_not_of(whitespace) - first + 1);

    std::size_t scheme_length = 0;
    if (starts_with_ignoring_case(url, "https://")) {
        scheme_length = 8;
    } else if (starts_with_ignoring_case(url, "http://")) {
        scheme_length = 7;
    } else {
        error = "the server URL must start with https:// or http://";
        return false;
    }
    // The host is what follows the scheme up to the first '/'.
    const std::size_t host_end = url.find('/', scheme_length);
    if (host_end == scheme_length || scheme_length == url.size()) {
        error = "the server URL has no host name";
        return false;
    }
    for (char c : url) {
        if (static_cast<unsigned char>(c) <= ' ' || c == '?' || c == '#') {
            error = "the server URL must not contain spaces, control characters, '?' or '#'";
            return false;
        }
    }
    if (url[url.size() - 1] != '/') {
        url += '/';
    }
    return true;
}

}  // namespace igwl
