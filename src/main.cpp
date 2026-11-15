#include "gui.h"
#include "http.h"
#include "platform.h"
#include "sync.h"

#include "igwl_config.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>

namespace {

// The platform the binary was compiled for, resolved at compile time so that
// a quick run of any artifact tells which build it is.
const char* target_platform() {
#if defined(_WIN64)
    return "windows-x86_64";
#elif defined(_WIN32)
    return "windows-x86";
#elif defined(__linux__) && defined(__x86_64__)
    return "linux-x86_64";
#elif defined(__linux__) && defined(__i386__)
    return "linux-x86";
#else
    return "unknown";
#endif
}

const char kUsage[] =
    "usage: igwlegacy-launcher [options]\n"
    "\n"
    "Without options, opens the launcher window.\n"
    "\n"
    "  --version          print the version and exit\n"
    "  --headless         sync without a window, printing progress to stdout\n"
    "  --no-launch        only sync, do not start the game afterwards\n"
    "  --dir PATH         directory to sync (default: the launcher's directory)\n"
    "  --base-url URL     server to sync from (default: " IGWL_BASE_URL ")\n"
    "  --public-key HEX   Ed25519 key the manifest must be signed with\n"
    "                     (default: the key compiled into this build)\n"
    "  --help             show this text\n";

// Prints the sync's progress as plain lines for --headless runs.
class ConsoleObserver : public igwl::SyncObserver {
public:
    ConsoleObserver() : last_percent_(-1) {}

    void log(const std::string& line) override {
        std::cout << line << std::endl;
    }

    void status(const std::string& text) override {
        std::cout << "== " << text << std::endl;
        last_percent_ = -1;
    }

    // Only every 10% of overall progress, to keep logs readable.
    void progress(double, double overall) override {
        const int percent = static_cast<int>(overall * 10) * 10;
        if (percent != last_percent_ && overall > 0) {
            last_percent_ = percent;
            std::cout << "   " << percent << "%" << std::endl;
        }
    }

private:
    int last_percent_;
};

int run_headless(const igwl::SyncOptions& options, bool launch) {
    ConsoleObserver observer;
    std::atomic<bool> cancel(false);
    const igwl::SyncResult result = igwl::run_sync(options, observer, cancel);
    if (!result.ok) {
        std::cerr << "error: " << result.error << std::endl;
        return 1;
    }
    if (launch && !result.launch.empty()) {
        std::string error;
        if (!igwl::platform::launch(result.launch, options.root, error)) {
            std::cerr << "error: " << error << std::endl;
            return 1;
        }
        std::cout << "Started " << result.launch.u8string() << std::endl;
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    igwl::SyncOptions options;
    options.base_url = IGWL_BASE_URL;
    options.public_key_hex = IGWL_MANIFEST_PUBLIC_KEY;
    bool headless = false;
    bool launch = true;
    bool root_given = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        // Options taking a value; a missing value is a usage error.
        const bool has_value = i + 1 < argc;
        if (arg == "--version" || arg == "--help" || arg == "-h") {
            igwl::platform::attach_parent_console();
            if (arg == "--version") {
                std::cout << "igwlegacy-launcher " << IGWL_VERSION << " (" << target_platform() << ")\n";
            } else {
                std::cout << kUsage;
            }
            return 0;
        } else if (arg == "--headless") {
            headless = true;
        } else if (arg == "--no-launch") {
            launch = false;
        } else if (arg == "--dir" && has_value) {
            // argv is UTF-8 on Linux; on Windows it is in the ANSI code page,
            // so non-ASCII directories should rather be given via the default.
            options.root = fs::absolute(fs::path(argv[++i]));
            root_given = true;
        } else if (arg == "--base-url" && has_value) {
            options.base_url = argv[++i];
        } else if (arg == "--public-key" && has_value) {
            options.public_key_hex = argv[++i];
        } else {
            igwl::platform::attach_parent_console();
            std::cerr << "unknown or incomplete option: " << arg << "\n\n" << kUsage;
            return 2;
        }
    }

    if (headless) {
        igwl::platform::attach_parent_console();
    }

    try {
        options.launcher = igwl::platform::executable_path();
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << std::endl;
        return 1;
    }
    // The game lives next to the launcher by default.
    if (!root_given) {
        options.root = options.launcher.parent_path();
    }

    igwl::http_global_init();

    return headless ? run_headless(options, launch) : igwl::run_gui(options, launch);
}
