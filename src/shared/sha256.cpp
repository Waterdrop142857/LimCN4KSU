#include "shared/sha256.h"

#include <cstring>

namespace bybus::shared {
namespace {

constexpr uint32_t kRound[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
    0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
    0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
    0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

inline uint32_t rotate_right(uint32_t value, int bits) {
    return (value >> bits) | (value << (32 - bits));
}

inline void store_be32(uint8_t *out, uint32_t value) {
    out[0] = static_cast<uint8_t>(value >> 24);
    out[1] = static_cast<uint8_t>(value >> 16);
    out[2] = static_cast<uint8_t>(value >> 8);
    out[3] = static_cast<uint8_t>(value);
}

inline uint32_t load_be32(const uint8_t *in) {
    return (static_cast<uint32_t>(in[0]) << 24) | (static_cast<uint32_t>(in[1]) << 16) |
           (static_cast<uint32_t>(in[2]) << 8) | static_cast<uint32_t>(in[3]);
}

} // namespace

Sha256::Sha256() {
    state_[0] = 0x6a09e667u;
    state_[1] = 0xbb67ae85u;
    state_[2] = 0x3c6ef372u;
    state_[3] = 0xa54ff53au;
    state_[4] = 0x510e527fu;
    state_[5] = 0x9b05688cu;
    state_[6] = 0x1f83d9abu;
    state_[7] = 0x5be0cd19u;
    std::memset(buffer_, 0, sizeof(buffer_));
}

void Sha256::compress(const uint8_t block[64]) {
    uint32_t words[64];
    for (int i = 0; i < 16; ++i) words[i] = load_be32(block + i * 4);
    for (int i = 16; i < 64; ++i) {
        const uint32_t s0 = rotate_right(words[i - 15], 7) ^ rotate_right(words[i - 15], 18) ^
                            (words[i - 15] >> 3);
        const uint32_t s1 = rotate_right(words[i - 2], 17) ^ rotate_right(words[i - 2], 19) ^
                            (words[i - 2] >> 10);
        words[i] = words[i - 16] + s0 + words[i - 7] + s1;
    }
    uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
    uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];
    for (int i = 0; i < 64; ++i) {
        const uint32_t s1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
        const uint32_t choice = (e & f) ^ (~e & g);
        const uint32_t temp1 = h + s1 + choice + kRound[i] + words[i];
        const uint32_t s0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
        const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t temp2 = s0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
}

void Sha256::update(const void *data, size_t size) {
    if (finished_) return;
    const auto *bytes = static_cast<const uint8_t *>(data);
    length_ += size;
    if (buffered_ != 0) {
        const size_t take = size < 64 - buffered_ ? size : 64 - buffered_;
        std::memcpy(buffer_ + buffered_, bytes, take);
        buffered_ += take;
        bytes += take;
        size -= take;
        if (buffered_ == 64) {
            compress(buffer_);
            buffered_ = 0;
        }
    }
    while (size >= 64) {
        compress(bytes);
        bytes += 64;
        size -= 64;
    }
    if (size != 0) {
        std::memcpy(buffer_ + buffered_, bytes, size);
        buffered_ += size;
    }
}

void Sha256::update(const std::string &text) { update(text.data(), text.size()); }

void Sha256::finish(uint8_t out[32]) {
    if (finished_) return;
    const uint64_t bits = length_ * 8;
    uint8_t padding[72] = {0x80};
    size_t padding_size = (buffered_ < 56) ? 56 - buffered_ : 120 - buffered_;
    update(padding, padding_size);
    uint8_t length_bytes[8];
    for (int i = 0; i < 8; ++i) length_bytes[7 - i] = static_cast<uint8_t>(bits >> (i * 8));
    update(length_bytes, sizeof(length_bytes));
    for (int i = 0; i < 8; ++i) store_be32(out + i * 4, state_[i]);
    finished_ = true;
}

void Sha256::hash(const void *data, size_t size, uint8_t out[32]) {
    Sha256 context;
    context.update(data, size);
    context.finish(out);
}

std::string Sha256::hex(const void *data, size_t size) {
    uint8_t digest[32];
    hash(data, size, digest);
    static const char kAlphabet[] = "0123456789abcdef";
    std::string text;
    text.resize(64);
    for (size_t i = 0; i < 32; ++i) {
        text[i * 2] = kAlphabet[digest[i] >> 4];
        text[i * 2 + 1] = kAlphabet[digest[i] & 0x0F];
    }
    return text;
}

std::string Sha256::hex(const std::string &text) { return hex(text.data(), text.size()); }

bool is_sha256_hex(const std::string &text) {
    if (text.size() != 64) return false;
    for (const char c : text) {
        const bool digit = c >= '0' && c <= '9';
        const bool lower = c >= 'a' && c <= 'f';
        if (!digit && !lower) return false;
    }
    return true;
}

std::string short_id(const std::string &hex, size_t characters) {
    if (characters > hex.size()) characters = hex.size();
    return hex.substr(0, characters);
}

} // namespace bybus::shared
