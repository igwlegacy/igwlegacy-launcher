// Brings a local directory in line with the signed manifest on the server.
//
// 1. Download manifest.json and manifest.json.sig, verify the signature.
// 2. Check every listed file locally: missing, wrong size or wrong SHA-256
//    means it has to be downloaded. Hashes of unchanged files (same size and
//    modification time as last time) are taken from the state file instead
//    of re-reading gigabytes on every start.
// 3. Download what is needed into a temporary file, verify size and hash,
//    and only then move it into place, so an interrupted sync never leaves a
//    truncated or corrupt file behind. The server stores content only, as
//    chunks named after their hash; files split into several chunks are
//    reassembled in that temporary file, each chunk verified on its own as
//    it arrives. Installed and already present files get the modification
//    time and executable bit the manifest lists.
// 4. Delete files that an earlier manifest installed but the current one no
//    longer lists. Files the launcher never installed (saves, settings, ...)
//    are never touched.
#ifndef IGWL_SYNC_H
#define IGWL_SYNC_H

#include "fs.h"

#include <atomic>
#include <string>
#include <vector>

namespace igwl {

struct SyncOptions {
    std::string base_url;        // manifest.json, manifest.json.sig, chunks/ below it
    std::string public_key_hex;  // Ed25519 key manifest.json.sig must verify against
    fs::path root;               // local directory to sync
    // The running launcher and its config file: never overwritten or
    // deleted, even if the manifest lists them.
    std::vector<fs::path> protected_files;
};

// Receives progress from the sync. Called on the sync's own thread.
class SyncObserver {
public:
    virtual ~SyncObserver() {}

    // A line for the log.
    virtual void log(const std::string& line) = 0;

    // What is happening right now ("Checking files", "Downloading x", ...).
    virtual void status(const std::string& text) = 0;

    // Progress of the current item and of the whole phase, both 0..1.
    virtual void progress(double current, double overall) = 0;
};

struct SyncResult {
    bool ok;
    bool cancelled;
    std::string error;    // set if !ok && !cancelled
    fs::path launch;      // program to start, empty if the manifest has none
};

// Runs the whole sync; blocks until done, failed or cancelled. Setting
// `cancel` from another thread stops it at the next opportunity.
SyncResult run_sync(const SyncOptions& options, SyncObserver& observer,
                    const std::atomic<bool>& cancel);

}  // namespace igwl

#endif
