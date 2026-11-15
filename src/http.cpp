#include "http.h"

#include "platform.h"

#include <curl/curl.h>

#include <cstdio>

namespace igwl {

namespace {

// Carries the callbacks and their outcome through curl's void* user data.
struct Transfer {
    const HttpClient::Sink* sink;
    const HttpClient::Progress* progress;
    bool cancelled;
    bool sink_failed;
};

size_t write_callback(char* data, size_t size, size_t count, void* user) {
    Transfer* transfer = static_cast<Transfer*>(user);
    const size_t bytes = size * count;
    if (!(*transfer->sink)(data, bytes)) {
        transfer->sink_failed = true;
        // Anything other than `bytes` makes curl fail with CURLE_WRITE_ERROR.
        return 0;
    }
    return bytes;
}

int progress_callback(void* user, curl_off_t total, curl_off_t received, curl_off_t, curl_off_t) {
    Transfer* transfer = static_cast<Transfer*>(user);
    if (*transfer->progress &&
        !(*transfer->progress)(static_cast<std::uint64_t>(received),
                               static_cast<std::uint64_t>(total))) {
        transfer->cancelled = true;
        // Non-zero aborts the transfer with CURLE_ABORTED_BY_CALLBACK.
        return 1;
    }
    return 0;
}

}  // namespace

void http_global_init() {
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

HttpClient::HttpClient() : curl_(curl_easy_init()) {
    if (!curl_) {
        throw std::runtime_error("could not initialise libcurl");
    }
}

HttpClient::~HttpClient() {
    curl_easy_cleanup(static_cast<CURL*>(curl_));
}

void HttpClient::get(const std::string& url, const Sink& sink, const Progress& progress) {
    CURL* curl = static_cast<CURL*>(curl_);
    // Clears the options of the previous request but keeps the connection
    // cache and TLS session cache.
    curl_easy_reset(curl);

    Transfer transfer = {&sink, &progress, false, false};
    char error[CURL_ERROR_SIZE] = {0};

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "igwlegacy-launcher/" IGWL_VERSION);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
    // A redirect must never downgrade an https URL to plain http.
    if (url.compare(0, 8, "https://") == 0) {
        curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
    } else {
        curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    }
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
    // Give up on a stalled connection (less than 1 byte/s for 60 seconds)
    // rather than hanging forever; there is no overall timeout, as large
    // files on slow lines legitimately take long.
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);
    // Threads other than the main one must not get SIGPIPE/SIGALRM.
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    const std::string ca_bundle = platform::ca_bundle();
    if (!ca_bundle.empty()) {
        curl_easy_setopt(curl, CURLOPT_CAINFO, ca_bundle.c_str());
    }
#ifdef _WIN32
    // Schannel checks revocation by default and fails hard when the CRL/OCSP
    // server cannot be reached (captive portals, strict firewalls); only an
    // actually revoked certificate should be an error.
    curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, static_cast<long>(CURLSSLOPT_REVOKE_BEST_EFFORT));
#endif

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &transfer);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_callback);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &transfer);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);

    const CURLcode result = curl_easy_perform(curl);

    if (transfer.cancelled) {
        throw Cancelled();
    }
    if (transfer.sink_failed) {
        throw SinkAborted(url);
    }
    if (result != CURLE_OK) {
        throw std::runtime_error(url + ": " + (error[0] ? error : curl_easy_strerror(result)));
    }

    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    if (status != 200) {
        throw std::runtime_error(url + ": HTTP status " + std::to_string(status));
    }
}

std::string HttpClient::get_string(const std::string& url, std::size_t max_size,
                                   const Progress& progress) {
    std::string body;
    try {
        get(url,
            [&body, max_size](const char* data, std::size_t size) {
                if (body.size() + size > max_size) {
                    return false;
                }
                body.append(data, size);
                return true;
            },
            progress);
    } catch (const SinkAborted&) {
        // The only reason this sink refuses data.
        throw std::runtime_error(url + " is larger than the allowed " +
                                 std::to_string(max_size) + " bytes");
    }
    return body;
}

std::string url_encode_path(const std::string& path) {
    static const char digits[] = "0123456789ABCDEF";
    std::string encoded;
    for (unsigned char c : path) {
        const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                                (c >= '0' && c <= '9') || c == '-' || c == '.' ||
                                c == '_' || c == '~' || c == '/';
        if (unreserved) {
            encoded.push_back(static_cast<char>(c));
        } else {
            encoded.push_back('%');
            encoded.push_back(digits[c >> 4]);
            encoded.push_back(digits[c & 0x0f]);
        }
    }
    return encoded;
}

}  // namespace igwl
