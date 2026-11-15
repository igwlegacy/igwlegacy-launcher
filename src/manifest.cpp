#include "manifest.h"

#include <nlohmann/json.hpp>

#include <cctype>
#include <set>
#include <stdexcept>

namespace igwl {

const char* const kStateDirectory = ".igwl-launcher";

namespace {

// Limits that no legitimate manifest comes close to, but which keep a broken
// one from making the launcher allocate or loop excessively.
const std::size_t kMaxPathLength = 1024;
const std::size_t kMaxFiles = 1000000;
const std::size_t kMaxChunks = 10000000;

// Latest accepted modification time, around the year 2242: well past
// anything real, and small enough that converting it to any platform's file
// time (down to nanoseconds in 64 bits) cannot overflow. Must match
// MAX_MTIME in tools/manifest.py.
const std::uint64_t kMaxMtime = 1ull << 33;

// The manifest format this launcher understands; see manifest.h. Must match
// MANIFEST_FORMAT in tools/manifest.py.
const int kManifestFormat = 2;

std::string ascii_lower(std::string text) {
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return text;
}

// Device names Windows resolves regardless of directory and extension, so
// "aux.txt" opens the AUX device instead of a file.
bool is_windows_device_name(const std::string& segment) {
    const std::string stem = ascii_lower(segment.substr(0, segment.find('.')));
    if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul") {
        return true;
    }
    if (stem.size() == 4 && (stem.compare(0, 3, "com") == 0 || stem.compare(0, 3, "lpt") == 0) &&
        stem[3] >= '0' && stem[3] <= '9') {
        return true;
    }
    return false;
}

bool is_lower_hex(const std::string& text) {
    for (char c : text) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return true;
}

bool is_sha256_hex(const nlohmann::json& value) {
    return value.is_string() && value.get_ref<const std::string&>().size() == 64 &&
           is_lower_hex(value.get_ref<const std::string&>());
}

// Parses the "chunks" list of `file`'s JSON entry into `file.chunks`, adding
// their number to `total_chunks`.
void parse_chunks(const nlohmann::json& chunks, ManifestFile& file, std::size_t& total_chunks) {
    if (!chunks.is_array() || chunks.empty()) {
        throw std::runtime_error("manifest entry \"" + file.path + "\" has a malformed chunk list");
    }
    total_chunks += chunks.size();
    if (total_chunks > kMaxChunks) {
        throw std::runtime_error("manifest lists too many chunks");
    }
    std::uint64_t sum = 0;
    for (const nlohmann::json& entry : chunks) {
        if (!entry.is_object() || !entry.contains("size") || !entry["size"].is_number_unsigned() ||
            !entry.contains("sha256") || !is_sha256_hex(entry["sha256"])) {
            throw std::runtime_error("manifest entry \"" + file.path + "\" has a malformed chunk: " +
                                     entry.dump());
        }
        ManifestChunk chunk;
        chunk.size = entry["size"].get<std::uint64_t>();
        chunk.sha256 = entry["sha256"].get<std::string>();
        // Checked against `size` before adding, so the sum cannot overflow
        // and wrap around to a matching value.
        if (chunk.size == 0 || chunk.size > file.size - sum) {
            throw std::runtime_error("manifest entry \"" + file.path +
                                     "\" has chunks that do not add up to its size");
        }
        sum += chunk.size;
        file.chunks.push_back(chunk);
    }
    if (sum != file.size) {
        throw std::runtime_error("manifest entry \"" + file.path +
                                 "\" has chunks that do not add up to its size");
    }
}

}  // namespace

std::string chunk_url_path(const ManifestChunk& chunk) {
    // Plain lowercase hex, nothing to encode.
    return "chunks/" + chunk.sha256;
}

bool is_valid_manifest_path(const std::string& path, std::string& reason) {
    if (path.empty()) {
        reason = "empty path";
        return false;
    }
    if (path.size() > kMaxPathLength) {
        reason = "path too long";
        return false;
    }
    for (unsigned char c : path) {
        // Control characters, and characters Windows does not allow in file
        // names; ':' would otherwise also allow drive letters and NTFS
        // alternate data streams, '\\' a second kind of separator.
        if (c < 0x20 || c == 0x7f || c == '\\' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|') {
            reason = "forbidden character";
            return false;
        }
    }
    if (path[0] == '/') {
        reason = "absolute path";
        return false;
    }

    std::size_t start = 0;
    bool first = true;
    for (;;) {
        const std::size_t end = path.find('/', start);
        const std::string segment = path.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (segment.empty()) {
            reason = "empty path component";
            return false;
        }
        if (segment == "." || segment == "..") {
            reason = "relative path component";
            return false;
        }
        // Windows silently strips trailing dots and spaces, which would make
        // two different manifest paths refer to the same file.
        if (segment[segment.size() - 1] == '.' || segment[segment.size() - 1] == ' ') {
            reason = "component ends in a dot or space";
            return false;
        }
        if (is_windows_device_name(segment)) {
            reason = "reserved Windows device name";
            return false;
        }
        if (first && ascii_lower(segment) == kStateDirectory) {
            reason = "inside the launcher's state directory";
            return false;
        }
        first = false;
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return true;
}

Manifest parse_manifest(const std::string& text) {
    nlohmann::json document;
    try {
        document = nlohmann::json::parse(text);
    } catch (const nlohmann::json::exception& error) {
        throw std::runtime_error(std::string("manifest is not valid JSON: ") + error.what());
    }

    if (!document.is_object()) {
        throw std::runtime_error("manifest is not a JSON object");
    }
    if (!document.contains("format") || document["format"] != kManifestFormat) {
        throw std::runtime_error("unsupported manifest format (this launcher understands format " +
                                 std::to_string(kManifestFormat) + ")");
    }

    const nlohmann::json& files = document["files"];
    if (!files.is_array()) {
        throw std::runtime_error("manifest has no \"files\" array");
    }
    if (files.size() > kMaxFiles) {
        throw std::runtime_error("manifest lists too many files");
    }

    Manifest manifest;
    // Case-folded paths seen so far: names differing only in case collide on
    // Windows (and on case-insensitive file systems elsewhere).
    std::set<std::string> seen;
    std::size_t total_chunks = 0;
    for (const nlohmann::json& entry : files) {
        if (!entry.is_object() || !entry.contains("path") || !entry["path"].is_string() ||
            !entry.contains("size") || !entry["size"].is_number_unsigned() ||
            !entry.contains("sha256") || !entry["sha256"].is_string() ||
            !entry.contains("mtime") || !entry["mtime"].is_number_unsigned()) {
            throw std::runtime_error("manifest has a malformed file entry: " + entry.dump());
        }

        ManifestFile file;
        file.path = entry["path"].get<std::string>();
        file.size = entry["size"].get<std::uint64_t>();
        file.sha256 = entry["sha256"].get<std::string>();
        if (entry["mtime"].get<std::uint64_t>() > kMaxMtime) {
            throw std::runtime_error("manifest entry \"" + file.path + "\" has an implausible mtime");
        }
        file.mtime = static_cast<std::int64_t>(entry["mtime"].get<std::uint64_t>());
        file.executable = entry.contains("executable") && entry["executable"] == true;

        std::string reason;
        if (!is_valid_manifest_path(file.path, reason)) {
            throw std::runtime_error("manifest path \"" + file.path + "\" rejected: " + reason);
        }
        if (file.sha256.size() != 64 || !is_lower_hex(file.sha256)) {
            throw std::runtime_error("manifest entry \"" + file.path + "\" has a malformed sha256");
        }
        if (!seen.insert(ascii_lower(file.path)).second) {
            throw std::runtime_error("manifest lists \"" + file.path + "\" more than once");
        }
        if (entry.contains("chunks")) {
            parse_chunks(entry["chunks"], file, total_chunks);
        } else if (file.size > 0) {
            // Stored whole: the single chunk is the file itself.
            ManifestChunk whole;
            whole.size = file.size;
            whole.sha256 = file.sha256;
            file.chunks.push_back(whole);
            if (++total_chunks > kMaxChunks) {
                throw std::runtime_error("manifest lists too many chunks");
            }
        }
        manifest.files.push_back(file);
    }

    if (document.contains("launch")) {
        const nlohmann::json& launch = document["launch"];
        if (!launch.is_object()) {
            throw std::runtime_error("manifest \"launch\" is not an object");
        }
        for (nlohmann::json::const_iterator it = launch.begin(); it != launch.end(); ++it) {
            if (!it.value().is_string()) {
                throw std::runtime_error("manifest \"launch\" entry " + it.key() + " is not a string");
            }
            const std::string program = it.value().get<std::string>();
            if (!seen.count(ascii_lower(program))) {
                throw std::runtime_error("launch program \"" + program + "\" is not listed in the manifest's files");
            }
            manifest.launch[it.key()] = program;
        }
    }

    return manifest;
}

}  // namespace igwl
