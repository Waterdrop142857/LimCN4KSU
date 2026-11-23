// SHA-256 (FIPS 180-4). Shared by archive validation, snapshot binding and the
// runtime lookup index. No allocation, no locale, no I/O.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace bybus::shared {

class Sha256 {
public:
    Sha256();
    void update(const void *data, size_t size);
    void update(const std::string &text);
    // Writes 32 raw bytes; the object must not be reused afterwards.
    void finish(uint8_t out[32]);

    static void hash(const void *data, size_t size, uint8_t out[32]);
    static std::string hex(const void *data, size_t size);
    static std::string hex(const std::string &text);

private:
    void compress(const uint8_t block[64]);

    uint32_t state_[8];
    uint64_t length_ = 0;
    uint8_t buffer_[64];
    size_t buffered_ = 0;
    bool finished_ = false;
};

// Lowercase hex of exactly 64 characters.
bool is_sha256_hex(const std::string &text);
// Copies the first 16 bytes of the digest into a 32 character identifier.
std::string short_id(const std::string &hex, size_t characters = 16);

} // namespace bybus::shared
