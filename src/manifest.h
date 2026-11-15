// The manifest describing the files the server offers.
//
//   {
//     "format": 2,
//     "files": [
//       {"path": "bin/game.exe", "size": 1234, "sha256": "<64 hex>",
//        "mtime": 1731667200, "executable": true},
//       {"path": "data/big.pak", "size": 45000000, "sha256": "<64 hex>",
//        "mtime": 1731667200,
//        "chunks": [{"size": 20000000, "sha256": "<64 hex>"},
//                   {"size": 20000000, "sha256": "<64 hex>"},
//                   {"size": 5000000, "sha256": "<64 hex>"}]},
//       ...
//     ],
//     "launch": {"windows": "bin/game.exe", "linux": "bin/game.sh"}
//   }
//
// The server stores content only, as chunks/<sha256>; file names, the
// modification time and the executable bit exist only in the manifest.
// "size" and "sha256" always describe the whole file as installed, "mtime"
// is its modification time in seconds since the Unix epoch, which the
// launcher gives the installed file.
//
// The hosting provider limits the size of single files, so larger game
// files are split: their "chunks" list consecutive byte ranges of the file
// (plain, not compressed), and the launcher concatenates them in order. A
// file without "chunks" is a single chunk named after its own "sha256"; an
// empty one has no chunk at all.
//// "executable" is optional (default false) and only matters on Linux, where
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

// One piece of a file's content, as stored on the server.
struct ManifestChunk {
    std::uint64_t size;  // never 0
    std::string sha256;  // lowercase hex, also the chunk's name on the server
};

struct ManifestFile {
    std::string path;  // '/' separated, relative, validated by parse_manifest
    std::uint64_t size;
    std::string sha256;  // lowercase hex, of the whole file
    std::int64_t mtime;  // seconds since the Unix epoch
    bool executable;
    // The pieces to download and concatenate, in order, whose sizes add up
    // to `size`: a single one for a file the manifest lists without
    // "chunks", none for an empty file.
    std::vector<ManifestChunk> chunks;
};

struct Manifest {
    std::vector<ManifestFile> files;
    // Platform key ("windows", "linux") -> path of the program to start.
    std::map<std::string, std::string> launch;
};

// Parses and validates manifest.json. Throws std::runtime_error describing
// the first problem found; a manifest is either fully valid or rejected.
Manifest parse_manifest(const std::string& text);

// Where below the base URL the server keeps a chunk: chunks/<sha256>.
// tools/manifest.py lays out the publish directory the same way.
std::string chunk_url_path(const ManifestChunk& chunk);

// Whether `path` is acceptable as a manifest path on every platform; if not,
// `reason` says why. Exposed for the tests of the validation rules.
bool is_valid_manifest_path(const std::string& path, std::string& reason);

}  // namespace igwl

#endif
