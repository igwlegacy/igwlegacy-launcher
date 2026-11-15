#include "crypto.h"

#include <mbedtls/sha256.h>
#include <monocypher-ed25519.h>

namespace igwl {

struct Sha256::Impl {
    mbedtls_sha256_context context;
};

Sha256::Sha256() : impl_(new Impl) {
    mbedtls_sha256_init(&impl_->context);
    // 0 selects SHA-256 rather than SHA-224.
    mbedtls_sha256_starts(&impl_->context, 0);
}

Sha256::~Sha256() {
    mbedtls_sha256_free(&impl_->context);
    delete impl_;
}

void Sha256::update(const void* data, std::size_t size) {
    mbedtls_sha256_update(&impl_->context, static_cast<const unsigned char*>(data), size);
}

std::string Sha256::hex_digest() {
    unsigned char digest[32];
    mbedtls_sha256_finish(&impl_->context, digest);

    static const char digits[] = "0123456789abcdef";
    std::string hex;
    hex.reserve(64);
    for (unsigned char byte : digest) {
        hex.push_back(digits[byte >> 4]);
        hex.push_back(digits[byte & 0x0f]);
    }
    return hex;
}

namespace {

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}  // namespace

bool hex_decode(const std::string& hex, std::vector<std::uint8_t>& out) {
    if (hex.size() % 2 != 0) {
        return false;
    }
    out.clear();
    out.reserve(hex.size() / 2);
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        const int high = hex_value(hex[i]);
        const int low = hex_value(hex[i + 1]);
        if (high < 0 || low < 0) {
            return false;
        }
        out.push_back(static_cast<std::uint8_t>((high << 4) | low));
    }
    return true;
}

bool ed25519_verify(const std::vector<std::uint8_t>& public_key,
                    const std::vector<std::uint8_t>& signature,
                    const std::string& message) {
    if (public_key.size() != 32 || signature.size() != 64) {
        return false;
    }
    // crypto_ed25519_check is the RFC 8032 (SHA-512) flavour, unlike
    // Monocypher's default EdDSA which uses BLAKE2b, so signatures from any
    // standard Ed25519 implementation (tools/manifest.py included) verify.
    return crypto_ed25519_check(signature.data(), public_key.data(),
                                reinterpret_cast<const std::uint8_t*>(message.data()),
                                message.size()) == 0;
}

}  // namespace igwl
