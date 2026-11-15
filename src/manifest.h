// The manifest describing the files the server offers.
//
//   {
//     "format": 1,
//     "files": [
//       {"path": "bin/game.exe", "size": 1234, "sha256": "<64 hex>", "executable": true},
//       ...
//     ],
//     "launch": {"windows": "bin/game.exe", "linux": "bin/game.sh"}
//   }
//
// "executable" is optional (default false) and only matters on Linux, where
// the file gets its execute bits set. "launch" is optional as a whole and per
// platform; every program named there must also be listed in "files", so that
// only verified content is ever executed.
//
// manifest.json is signed as is: manifest.json.sig holds the hex encoded
// Ed25519 signature over its exact bytes. tools/manifest.py writes both.
#ifndef IGWL_MANIFEST_H
#define IGWL_MANIFEST_H

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace igwl {

// Directory below the sync root holding the launcher's own state; manifests
// may not place files in there.
extern const char* const kStateDirectory;

struct ManifestFile {
    std::string path;  // '/' separated, relative, validated by parse_manifest
    std::uint64_t size;
    std::string sha256;  // lowercase hex
    bool executable;
};

struct Manifest {
    std::vector<ManifestFile> files;
    // Platform key ("windows", "linux") -> path of the program to start.
    std::map<std::string, std::string> launch;
};

// Parses and validates manifest.json. Throws std::runtime_error describing
// the first problem found; a manifest is either fully valid or rejected.
Manifest parse_manifest(const std::string& text);

// Whether `path` is acceptable as a manifest path on every platform; if not,
// `reason` says why. Exposed for the tests of the validation rules.
bool is_valid_manifest_path(const std::string& path, std::string& reason);

}  // namespace igwl

#endif
