#include "sync.h"

#include "crypto.h"
#include "http.h"
#include "manifest.h"
#include "platform.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdio>
#include <map>
#include <set>
#include <stdexcept>
#include <system_error>
#include <vector>

namespace igwl {

namespace {

// Upper bounds for what is downloaded into memory; a real manifest with a
// million entries is still well below this.
const std::size_t kMaxManifestSize = 256u << 20;
const std::size_t kMaxSignatureSize = 1024;

// Read size when hashing local files.
const std::size_t kHashChunk = 1u << 20;

// What the state file remembers about a file the launcher installed or
// verified: if size and modification time are unchanged, so is the content.
struct KnownFile {
    std::uint64_t size;
    std::int64_t mtime;
    std::string sha256;
};

typedef std::map<std::string, KnownFile> State;

void check_cancel(const std::atomic<bool>& cancel) {
    if (cancel.load()) {
        throw Cancelled();
    }
}

std::string format_size(std::uint64_t bytes) {
    char text[32];
    if (bytes >= (1ull << 30)) {
        std::snprintf(text, sizeof text, "%.2f GiB", bytes / double(1ull << 30));
    } else if (bytes >= (1ull << 20)) {
        std::snprintf(text, sizeof text, "%.1f MiB", bytes / double(1ull << 20));
    } else if (bytes >= (1ull << 10)) {
        std::snprintf(text, sizeof text, "%.1f KiB", bytes / double(1ull << 10));
    } else {
        std::snprintf(text, sizeof text, "%llu B", static_cast<unsigned long long>(bytes));
    }
    return text;
}

// Limits progress updates to a rate the GUI can keep up with; every update
// costs a round trip through the FLTK lock.
class Throttle {
public:
    Throttle() : last_() {}

    bool due() {
        const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
        if (now - last_ < std::chrono::milliseconds(50)) {
            return false;
        }
        last_ = now;
        return true;
    }

private:
    std::chrono::steady_clock::time_point last_;
};

std::int64_t modification_time(const fs::path& file) {
    return static_cast<std::int64_t>(fs::last_write_time(file).time_since_epoch().count());
}

std::string trim(const std::string& text) {
    const char* whitespace = " \t\r\n";
    const std::size_t begin = text.find_first_not_of(whitespace);
    if (begin == std::string::npos) {
        return std::string();
    }
    return text.substr(begin, text.find_last_not_of(whitespace) - begin + 1);
}

// The state file is a cache: if it is missing or unreadable, the sync just
// re-hashes everything and nothing worse happens.
State load_state(const fs::path& file, SyncObserver& observer) {
    State state;
    std::error_code error;
    if (!fs::exists(file, error)) {
        return state;
    }
    try {
        fs::ifstream input(file, std::ios::binary);
        const nlohmann::json document = nlohmann::json::parse(input);
        for (nlohmann::json::const_iterator it = document.at("files").begin();
             it != document.at("files").end(); ++it) {
            KnownFile known;
            known.size = it.value().at("size").get<std::uint64_t>();
            known.mtime = it.value().at("mtime").get<std::int64_t>();
            known.sha256 = it.value().at("sha256").get<std::string>();
            state[it.key()] = known;
        }
    } catch (const std::exception& problem) {
        observer.log(std::string("Ignoring unreadable state file: ") + problem.what());
        state.clear();
    }
    return state;
}

// Written to a temporary file first and renamed over the old one, so a crash
// in the middle never leaves a truncated state file.
void save_state(const fs::path& file, const State& state) {
    nlohmann::json files = nlohmann::json::object();
    for (State::const_iterator it = state.begin(); it != state.end(); ++it) {
        files[it->first] = {
            {"size", it->second.size},
            {"mtime", it->second.mtime},
            {"sha256", it->second.sha256},
        };
    }
    const nlohmann::json document = {{"format", 1}, {"files", files}};

    const fs::path temporary = file.string() + ".tmp";
    {
        fs::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output << document.dump(1) << '\n';
        if (!output) {
            throw std::runtime_error("could not write " + temporary.u8string());
        }
    }
    fs::rename(temporary, file);
}

// SHA-256 of a local file, reporting the bytes read through `on_read`.
template <typename Callback>
std::string hash_file(const fs::path& file, const std::atomic<bool>& cancel, Callback on_read) {
    fs::ifstream input(file, std::ios::binary);
    if (!input) {
        throw std::runtime_error("could not open " + file.u8string());
    }
    Sha256 hash;
    std::vector<char> buffer(kHashChunk);
    while (input) {
        check_cancel(cancel);
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize got = input.gcount();
        if (got > 0) {
            hash.update(buffer.data(), static_cast<std::size_t>(got));
            on_read(static_cast<std::uint64_t>(got));
        }
    }
    if (input.bad()) {
        throw std::runtime_error("could not read " + file.u8string());
    }
    return hash.hex_digest();
}

// Removes directories that became empty after deleting a file, walking up
// towards (but never including) the sync root.
void remove_empty_parents(fs::path directory, const fs::path& root) {
    std::error_code error;
    while (directory != root && directory.has_relative_path() &&
           fs::is_directory(directory, error) && fs::is_empty(directory, error)) {
        if (!fs::remove(directory, error)) {
            break;
        }
        directory = directory.parent_path();
    }
}

// Whether `file` is the running launcher, which must neither be replaced
// (impossible while it runs on Windows) nor deleted.
bool is_launcher(const fs::path& file, const fs::path& launcher) {
    std::error_code error;
    return fs::equivalent(file, launcher, error);
}

class Sync {
public:
    Sync(const SyncOptions& options, SyncObserver& observer, const std::atomic<bool>& cancel)
        : options_(options),
          observer_(observer),
          cancel_(cancel),
          base_url_(options.base_url),
          state_dir_(options.root / kStateDirectory),
          state_file_(state_dir_ / "state.json"),
          download_file_(state_dir_ / "download.part") {
        if (base_url_.empty() || base_url_[base_url_.size() - 1] != '/') {
            base_url_ += '/';
        }
    }

    SyncResult run() {
        SyncResult result;
        result.ok = false;
        result.cancelled = false;
        try {
            const Manifest manifest = fetch_manifest();
            check_files(manifest);
            download_files();
            remove_stale_files(manifest);
            finish(manifest, result);
            result.ok = true;
        } catch (const Cancelled&) {
            result.cancelled = true;
        } catch (const std::exception& error) {
            result.error = error.what();
        }

        // Whatever happened, keep what was verified so far for next time.
        try {
            if (state_loaded_) {
                save_state(state_file_, state_);
            }
        } catch (const std::exception& error) {
            observer_.log(std::string("Could not save the state file: ") + error.what());
        }
        std::error_code ignored;
        fs::remove(download_file_, ignored);
        return result;
    }

private:
    // Step 1: download and verify the manifest.
    Manifest fetch_manifest() {
        std::vector<std::uint8_t> public_key;
        if (options_.public_key_hex.empty()) {
            throw std::runtime_error(
                "This launcher build has no manifest public key configured, so it "
                "cannot verify any update. Set IGWL_MANIFEST_PUBLIC_KEY when building.");
        }
        if (!hex_decode(options_.public_key_hex, public_key) || public_key.size() != 32) {
            throw std::runtime_error("The configured manifest public key is malformed.");
        }

        observer_.status("Downloading the file list");
        observer_.log("Fetching " + base_url_ + "manifest.json");
        Throttle throttle;
        const HttpClient::Progress progress = [this, &throttle](std::uint64_t received, std::uint64_t total) {
            if (total > 0 && throttle.due()) {
                observer_.progress(double(received) / total, 0.0);
            }
            return !cancel_.load();
        };
        const std::string text = http_.get_string(base_url_ + "manifest.json", kMaxManifestSize, progress);
        const std::string signature_hex =
            trim(http_.get_string(base_url_ + "manifest.json.sig", kMaxSignatureSize, progress));

        std::vector<std::uint8_t> signature;
        if (!hex_decode(signature_hex, signature) || signature.size() != 64) {
            throw std::runtime_error("manifest.json.sig is malformed");
        }
        // Verified before parsing, so the JSON parser only ever sees content
        // the publisher signed.
        if (!ed25519_verify(public_key, signature, text)) {
            throw std::runtime_error(
                "The signature of manifest.json is invalid. Refusing to sync: the "
                "server content was not published with the expected key.");
        }

        Manifest manifest = parse_manifest(text);
        std::uint64_t total = 0;
        for (const ManifestFile& file : manifest.files) {
            total += file.size;
        }
        observer_.log("Manifest verified: " + std::to_string(manifest.files.size()) + " files, " +
                      format_size(total));
        return manifest;
    }

    // Step 2: find out which files are missing or differ.
    void check_files(const Manifest& manifest) {
        observer_.status("Checking local files");
        fs::create_directories(state_dir_);
        state_ = load_state(state_file_, observer_);
        state_loaded_ = true;

        // First a cheap pass over sizes and the cache, collecting the files
        // whose content actually has to be hashed.
        std::vector<const ManifestFile*> to_hash;
        std::uint64_t hash_bytes = 0;
        for (const ManifestFile& file : manifest.files) {
            check_cancel(cancel_);
            const fs::path local = options_.root / fs::path(file.path);
            if (is_launcher(local, options_.launcher)) {
                observer_.log("Skipping " + file.path + ": that is the running launcher itself");
                continue;
            }

            std::error_code error;
            const fs::file_status status = fs::status(local, error);
            if (!fs::is_regular_file(status) || fs::file_size(local, error) != file.size || error) {
                state_.erase(file.path);
                pending_.push_back(&file);
                continue;
            }

            State::const_iterator known = state_.find(file.path);
            if (known != state_.end() && known->second.size == file.size &&
                known->second.sha256 == file.sha256 &&
                known->second.mtime == modification_time(local)) {
                ensure_mode(file, local);
                continue;
            }
            to_hash.push_back(&file);
            hash_bytes += file.size;
        }

        // Then the expensive pass, with progress since this reads every byte.
        std::uint64_t hashed = 0;
        Throttle throttle;
        for (std::size_t i = 0; i < to_hash.size(); ++i) {
            const ManifestFile& file = *to_hash[i];
            const fs::path local = options_.root / fs::path(file.path);
            observer_.status("Checking " + file.path);
            std::uint64_t done = 0;
            const std::string digest = hash_file(local, cancel_, [&](std::uint64_t bytes) {
                done += bytes;
                hashed += bytes;
                if (throttle.due()) {
                    observer_.progress(file.size ? double(done) / file.size : 1.0,
                                       hash_bytes ? double(hashed) / hash_bytes : 1.0);
                }
            });
            if (digest == file.sha256) {
                remember(file, local);
                ensure_mode(file, local);
            } else {
                state_.erase(file.path);
                pending_.push_back(&file);
            }
        }

        if (pending_.empty()) {
            observer_.log("All files are up to date");
        } else {
            std::uint64_t bytes = 0;
            for (const ManifestFile* file : pending_) {
                bytes += file->size;
            }
            observer_.log(std::to_string(pending_.size()) +
                          (pending_.size() == 1 ? " file needs" : " files need") +
                          " to be downloaded (" +
                          format_size(bytes) + ")");
        }
    }

    // Step 3: download whatever step 2 found missing or different.
    void download_files() {
        std::uint64_t total = 0;
        for (const ManifestFile* file : pending_) {
            total += file->size;
        }

        std::uint64_t completed = 0;
        Throttle throttle;
        std::chrono::steady_clock::time_point last_save = std::chrono::steady_clock::now();

        for (std::size_t i = 0; i < pending_.size(); ++i) {
            const ManifestFile& file = *pending_[i];
            const fs::path local = options_.root / fs::path(file.path);
            observer_.status("Downloading " + file.path + " (" + std::to_string(i + 1) + "/" +
                             std::to_string(pending_.size()) + ")");

            download(file, [&](std::uint64_t received) {
                if (throttle.due()) {
                    observer_.progress(file.size ? double(received) / file.size : 1.0,
                                       total ? double(completed + received) / total : 1.0);
                }
            });

            // Only now, with size and hash verified, does the file replace
            // the old one.
            fs::create_directories(local.parent_path());
            std::error_code error;
            fs::rename(download_file_, local, error);
            if (error) {
                throw std::runtime_error("could not replace " + file.path + ": " + error.message() +
                                         " (is the game still running?)");
            }
            ensure_mode(file, local);
            remember(file, local);
            completed += file.size;
            observer_.log("Updated " + file.path);

            // Saving the state now and then means an interrupted sync does
            // not have to re-hash what it already downloaded.
            if (std::chrono::steady_clock::now() - last_save > std::chrono::seconds(5)) {
                save_state(state_file_, state_);
                last_save = std::chrono::steady_clock::now();
            }
        }
        observer_.progress(1.0, 1.0);
    }

    // Downloads one file into download_file_ and verifies it against the
    // manifest. `on_progress` gets the bytes received so far.
    template <typename Callback>
    void download(const ManifestFile& file, Callback on_progress) {
        fs::ofstream output(download_file_, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error("could not create " + download_file_.u8string());
        }

        Sha256 hash;
        std::uint64_t received = 0;
        bool write_failed = false;
        try {
            http_.get(
                base_url_ + "files/" + url_encode_path(file.path),
                [&](const char* data, std::size_t size) {
                    // Refuse more data than announced right away instead of
                    // filling the disk with whatever the server sends.
                    if (received + size > file.size) {
                        return false;
                    }
                    output.write(data, static_cast<std::streamsize>(size));
                    if (!output) {
                        write_failed = true;
                        return false;
                    }
                    hash.update(data, size);
                    received += size;
                    return true;
                },
                [&](std::uint64_t, std::uint64_t) {
                    on_progress(received);
                    return !cancel_.load();
                });
        } catch (const SinkAborted&) {
            // The sink above refused data: either the disk write failed
            // (reported below) or the server sent more than announced.
            if (!write_failed) {
                throw std::runtime_error("the server sent more data for " + file.path +
                                         " than manifest.json lists; the server copy is "
                                         "inconsistent with manifest.json");
            }
        }
        output.close();

        if (write_failed || output.fail()) {
            throw std::runtime_error("could not write " + download_file_.u8string() +
                                     " (disk full?)");
        }
        if (received != file.size || hash.hex_digest() != file.sha256) {
            throw std::runtime_error("downloaded " + file.path +
                                     " does not match the manifest; the server copy is "
                                     "inconsistent with manifest.json");
        }
    }

    // Step 4: delete what earlier syncs installed but the manifest dropped.
    void remove_stale_files(const Manifest& manifest) {
        std::set<std::string> listed;
        for (const ManifestFile& file : manifest.files) {
            listed.insert(file.path);
        }

        std::vector<std::string> stale;
        for (State::const_iterator it = state_.begin(); it != state_.end(); ++it) {
            if (!listed.count(it->first)) {
                stale.push_back(it->first);
            }
        }

        for (const std::string& path : stale) {
            state_.erase(path);
            // The state file is local and trusted, but a path that would not
            // pass as a manifest path is never acted upon regardless.
            std::string reason;
            if (!is_valid_manifest_path(path, reason)) {
                continue;
            }
            const fs::path local = options_.root / fs::path(path);
            std::error_code error;
            if (!fs::is_regular_file(local, error) || is_launcher(local, options_.launcher)) {
                continue;
            }
            if (fs::remove(local, error)) {
                observer_.log("Removed " + path);
                remove_empty_parents(local.parent_path(), options_.root);
            } else if (error) {
                observer_.log("Could not remove " + path + ": " + error.message());
            }
        }
    }

    void finish(const Manifest& manifest, SyncResult& result) {
        std::map<std::string, std::string>::const_iterator program =
            manifest.launch.find(platform::launch_key());
        if (program != manifest.launch.end()) {
            result.launch = options_.root / fs::path(program->second);
        }
        observer_.status("Up to date");
    }

    void remember(const ManifestFile& file, const fs::path& local) {
        KnownFile known;
        known.size = file.size;
        known.mtime = modification_time(local);
        known.sha256 = file.sha256;
        state_[file.path] = known;
    }

    void ensure_mode(const ManifestFile& file, const fs::path& local) {
        if (file.executable) {
            platform::make_executable(local);
        }
    }

    const SyncOptions& options_;
    SyncObserver& observer_;
    const std::atomic<bool>& cancel_;
    std::string base_url_;
    const fs::path state_dir_;
    const fs::path state_file_;
    const fs::path download_file_;

    HttpClient http_;
    State state_;
    bool state_loaded_ = false;
    std::vector<const ManifestFile*> pending_;
};

}  // namespace

SyncResult run_sync(const SyncOptions& options, SyncObserver& observer,
                    const std::atomic<bool>& cancel) {
    try {
        Sync sync(options, observer, cancel);
        return sync.run();
    } catch (const std::exception& error) {
        // Only the constructor can get here (e.g. curl failing to start).
        SyncResult result;
        result.ok = false;
        result.cancelled = false;
        result.error = error.what();
        return result;
    }
}

}  // namespace igwl
