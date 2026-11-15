// Hashing and signature verification used by the sync.
#ifndef IGWL_CRYPTO_H
#define IGWL_CRYPTO_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace igwl {

// Incremental SHA-256 (mbedTLS underneath), for hashing files while they are
// read or downloaded without holding them in memory.
class Sha256 {
public:
    Sha256();
    ~Sha256();

    void update(const void* data, std::size_t size);

    // Lowercase hex digest. The object must not be updated afterwards.
    std::string hex_digest();

private:
    Sha256(const Sha256&);
    Sha256& operator=(const Sha256&);

    // Opaque storage for mbedtls_sha256_context, keeping mbedTLS headers out
    // of every file that includes this one.
    struct Impl;
    Impl* impl_;
};

// Decodes hex (either case) into bytes; false on odd length or bad digits.
bool hex_decode(const std::string& hex, std::vector<std::uint8_t>& out);

// Checks an Ed25519 (RFC 8032) signature over `message`.
bool ed25519_verify(const std::vector<std::uint8_t>& public_key,
                    const std::vector<std::uint8_t>& signature,
                    const std::string& message);

}  // namespace igwl

#endif
