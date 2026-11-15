// The few things that differ between Windows and Linux.
#ifndef IGWL_PLATFORM_H
#define IGWL_PLATFORM_H

#include "fs.h"

#include <string>

namespace igwl {
namespace platform {

// Absolute path of the running launcher executable.
fs::path executable_path();

// The key selecting this platform's entry in the manifest's "launch" table:
// "windows" or "linux".
const char* launch_key();

// CA bundle for curl to verify servers against. Empty where curl's TLS
// backend uses the OS certificate store (Schannel on Windows). On Linux the
// location differs between distributions, so it is searched at runtime;
// SSL_CERT_FILE overrides the search.
std::string ca_bundle();

// Marks a file as executable (chmod +x). A no-op on Windows.
void make_executable(const fs::path& file);

// Starts `program` with `directory` as its working directory and returns
// without waiting for it. Returns false and fills `error` on failure.
bool launch(const fs::path& program, const fs::path& directory, std::string& error);

// Windows GUI executables have no console; this attaches to the one of the
// parent process (if any), so command line output such as --version shows
// up in the terminal the launcher was started from. Output redirected to a
// file or pipe is left alone. A no-op on Linux.
void attach_parent_console();

}  // namespace platform
}  // namespace igwl

#endif
