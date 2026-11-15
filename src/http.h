// Minimal blocking HTTP(S) GET on top of libcurl.
#ifndef IGWL_HTTP_H
#define IGWL_HTTP_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>

namespace igwl {

// Thrown when a transfer was aborted because the user cancelled it.
class Cancelled : public std::runtime_error {
public:
    Cancelled() : std::runtime_error("cancelled") {}
};

// Thrown by HttpClient::get when the sink returned false. The sink knows why
// it refused the data, so callers catch this to report the actual reason.
class SinkAborted : public std::runtime_error {
public:
    explicit SinkAborted(const std::string& url)
        : std::runtime_error("download of " + url + " aborted") {}
};

class HttpClient {
public:
    // Receives the body in chunks; return false to abort the transfer.
    typedef std::function<bool(const char* data, std::size_t size)> Sink;

    // Reports bytes received so far and the expected total (0 if unknown);
    // return false to cancel. Called from within the transfer, often.
    typedef std::function<bool(std::uint64_t received, std::uint64_t total)> Progress;

    HttpClient();
    ~HttpClient();

    // Fetches `url`, streaming the body into `sink`. Throws Cancelled when
    // `progress` asked to stop, SinkAborted when `sink` refused data and
    // std::runtime_error on any other failure (network, TLS, HTTP status).
    void get(const std::string& url, const Sink& sink, const Progress& progress);

    // Convenience wrapper: the whole body as a string, refusing anything
    // larger than `max_size` bytes.
    std::string get_string(const std::string& url, std::size_t max_size,
                           const Progress& progress);

private:
    HttpClient(const HttpClient&);
    HttpClient& operator=(const HttpClient&);

    // One easy handle for the whole sync, so that connections (and TLS
    // sessions) to the server are reused between files.
    void* curl_;
};

// Must be called once at startup, before any thread creates an HttpClient.
void http_global_init();

}  // namespace igwl

#endif
