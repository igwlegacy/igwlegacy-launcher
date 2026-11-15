#include "platform.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace igwl {
namespace platform {

#ifdef _WIN32

fs::path executable_path() {
    // MAX_PATH is not a real limit for long-path aware processes, so grow
    // the buffer until the name fits.
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
        const DWORD length = GetModuleFileNameW(NULL, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            throw std::runtime_error("GetModuleFileNameW failed");
        }
        if (length < buffer.size()) {
            return fs::path(std::wstring(buffer.data(), length));
        }
        buffer.resize(buffer.size() * 2);
    }
}

const char* launch_key() {
    return "windows";
}

std::string ca_bundle() {
    return std::string();
}

void make_executable(const fs::path&) {}

bool launch(const fs::path& program, const fs::path& directory, std::string& error) {
    // CreateProcessW may modify the command line buffer, so it has to be a
    // writable copy. The program path is quoted in case it contains spaces.
    std::wstring command_line = L"\"" + program.wstring() + L"\"";
    std::vector<wchar_t> buffer(command_line.begin(), command_line.end());
    buffer.push_back(L'\0');

    STARTUPINFOW startup;
    std::memset(&startup, 0, sizeof startup);
    startup.cb = sizeof startup;
    PROCESS_INFORMATION process;
    std::memset(&process, 0, sizeof process);

    if (!CreateProcessW(program.wstring().c_str(), buffer.data(), NULL, NULL, FALSE, 0, NULL,
                        directory.wstring().c_str(), &startup, &process)) {
        error = "could not start " + program.u8string() + " (error " +
                std::to_string(static_cast<unsigned long>(GetLastError())) + ")";
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

namespace {

// Whether a standard handle already leads somewhere (a file or pipe the
// caller redirected to, or a console); the C runtime binds those itself.
bool std_handle_usable(DWORD which) {
    const HANDLE handle = GetStdHandle(which);
    return handle != NULL && handle != INVALID_HANDLE_VALUE &&
           GetFileType(handle) != FILE_TYPE_UNKNOWN;
}

}  // namespace

void attach_parent_console() {
    // Checked before attaching, which makes the console's handles the
    // standard ones; redirected output must stay redirected.
    const bool stdout_usable = std_handle_usable(STD_OUTPUT_HANDLE);
    const bool stderr_usable = std_handle_usable(STD_ERROR_HANDLE);
    if (stdout_usable && stderr_usable) {
        return;
    }
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        // Reconnect the C streams that were bound to nothing at startup.
        if (!stdout_usable) {
            std::freopen("CONOUT$", "w", stdout);
        }
        if (!stderr_usable) {
            std::freopen("CONOUT$", "w", stderr);
        }
    }
}

#else  // Linux

fs::path executable_path() {
    return fs::read_symlink("/proc/self/exe");
}

const char* launch_key() {
    return "linux";
}

std::string ca_bundle() {
    const char* override_path = std::getenv("SSL_CERT_FILE");
    if (override_path && *override_path) {
        return override_path;
    }
    // The usual suspects, in the order curl's own configure script probes
    // them, plus the RHEL/CentOS 7 extracted bundle.
    static const char* const candidates[] = {
        "/etc/ssl/certs/ca-certificates.crt",                 // Debian, Ubuntu, Arch, Gentoo
        "/etc/pki/tls/certs/ca-bundle.crt",                   // Fedora, RHEL
        "/usr/share/ssl/certs/ca-bundle.crt",                 // old Red Hat
        "/usr/local/share/certs/ca-root-nss.crt",             // FreeBSD ports layout
        "/etc/ssl/cert.pem",                                  // Alpine, Void
        "/etc/ssl/ca-bundle.pem",                             // openSUSE
        "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",  // RHEL/CentOS 7
    };
    for (const char* candidate : candidates) {
        if (access(candidate, R_OK) == 0) {
            return candidate;
        }
    }
    // Nothing found: curl then fails verification with a clear message,
    // which is better than silently skipping it.
    return std::string();
}

void make_executable(const fs::path& file) {
    std::error_code ignored;
    fs::permissions(file,
                    fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                    fs::perm_options::add, ignored);
}

bool launch(const fs::path& program, const fs::path& directory, std::string& error) {
    // The child reports a failing chdir/exec through this pipe. On success
    // exec closes the write end (O_CLOEXEC), so the read below sees EOF.
    int status_pipe[2];
    if (pipe2(status_pipe, O_CLOEXEC) != 0) {
        error = std::string("pipe2 failed: ") + std::strerror(errno);
        return false;
    }

    const std::string program_name = program.string();
    const std::string directory_name = directory.string();

    const pid_t child = fork();
    if (child < 0) {
        error = std::string("fork failed: ") + std::strerror(errno);
        close(status_pipe[0]);
        close(status_pipe[1]);
        return false;
    }

    if (child == 0) {
        // Only async-signal-safe calls from here on: the launcher has other
        // threads, whose locks may be held by threads that no longer exist.
        close(status_pipe[0]);
        int failure = 0;
        if (chdir(directory_name.c_str()) != 0) {
            failure = errno;
        } else {
            execl(program_name.c_str(), program_name.c_str(), static_cast<char*>(NULL));
            failure = errno;
        }
        ssize_t ignored = write(status_pipe[1], &failure, sizeof failure);
        (void)ignored;
        _exit(127);
    }

    close(status_pipe[1]);
    int failure = 0;
    ssize_t received;
    do {
        received = read(status_pipe[0], &failure, sizeof failure);
    } while (received < 0 && errno == EINTR);
    close(status_pipe[0]);

    if (received == static_cast<ssize_t>(sizeof failure)) {
        // Reap the failed child, it has already exited.
        waitpid(child, NULL, 0);
        error = "could not start " + program_name + ": " + std::strerror(failure);
        return false;
    }
    return true;
}

void attach_parent_console() {}

#endif

}  // namespace platform
}  // namespace igwl
