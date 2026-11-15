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

}  // namespace

int main(int argc, char** argv) {
    std::cout << "igwlegacy-launcher " << IGWL_VERSION << " (" << target_platform() << ")\n";

    for (int i = 1; i < argc; ++i) {
        std::cout << "  arg " << i << ": " << std::string(argv[i]) << "\n";
    }

    return 0;
}
