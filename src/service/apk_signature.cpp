#include "service/apk_signature.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "shared/sha256.h"

namespace bybus::service {
namespace {

constexpr uint32_t kEocdSignature = 0x06054b50u;
constexpr uint64_t kMaxSigningBlock = 64ull * 1024 * 1024;
constexpr char kApkSigMagic[16] = {'A', 'P', 'K', ' ', 'S', 'i', 'g', ' ',
                                   'B', 'l', 'o', 'c', 'k', ' ', '4', '2'};
constexpr uint32_t kV2Id = 0x7109871au;
constexpr uint32_t kV3Id = 0xf05368c0u;

uint16_t le16(const std::vector<uint8_t> &bytes, size_t at) {
    if (at + 2 > bytes.size()) return 0;
    return static_cast<uint16_t>(bytes[at]) |
           (static_cast<uint16_t>(bytes[at + 1]) << 8);
}

uint32_t le32(const std::vector<uint8_t> &bytes, size_t at) {
    if (at + 4 > bytes.size()) return 0;
    return static_cast<uint32_t>(bytes[at]) |
           (static_cast<uint32_t>(bytes[at + 1]) << 8) |
           (static_cast<uint32_t>(bytes[at + 2]) << 16) |
           (static_cast<uint32_t>(bytes[at + 3]) << 24);
}

uint64_t le64(const std::vector<uint8_t> &bytes, size_t at) {
    if (at + 8 > bytes.size()) return 0;
    return static_cast<uint64_t>(le32(bytes, at)) |
           (static_cast<uint64_t>(le32(bytes, at + 4)) << 32);
}

bool read_at(std::ifstream &file, uint64_t offset, size_t size, std::vector<uint8_t> *out) {
    file.clear();
    file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!file) return false;
    out->assign(size, 0);
    if (size != 0) file.read(reinterpret_cast<char *>(out->data()),
                             static_cast<std::streamsize>(size));
    return file.gcount() == static_cast<std::streamsize>(size);
}

bool first_certificate(const std::vector<uint8_t> &value, std::vector<uint8_t> *certificate) {
    if (value.size() < 4) return false;
    const uint32_t signers_length = le32(value, 0);
    if (signers_length == 0 || signers_length > value.size() - 4) return false;
    size_t at = 4;
    uint32_t signer_count = 0;
    size_t first_signer = 0;
    size_t first_length = 0;
    while (at < 4ull + signers_length) {
        if (at + 4 > value.size()) return false;
        const uint32_t signer_length = le32(value, at);
        if (signer_length == 0 || signer_length > value.size() - at - 4) return false;
        if (signer_count == 0) {
            first_signer = at + 4;
            first_length = signer_length;
        }
        ++signer_count;
        at += 4ull + signer_length;
    }
    if (signer_count != 1 || at != 4ull + signers_length) return false;
    size_t cursor = first_signer;
    const size_t signer_end = first_signer + first_length;
    if (cursor + 4 > signer_end) return false;
    const uint32_t signed_length = le32(value, cursor);
    cursor += 4;
    if (signed_length == 0 || signed_length > signer_end - cursor) return false;
    const size_t signed_at = cursor;
    const size_t signed_end = cursor + signed_length;
    cursor = signed_end;
    if (cursor + 4 > signer_end) return false;
    const uint32_t signatures_length = le32(value, cursor);
    cursor += 4;
    if (signatures_length > signer_end - cursor) return false;
    cursor += signatures_length;
    if (cursor + 4 > signer_end) return false;
    const uint32_t key_length = le32(value, cursor);
    cursor += 4;
    if (key_length > signer_end - cursor) return false;
    cursor += key_length;
    if (cursor != signer_end) return false;

    size_t signed_cursor = signed_at;
    if (signed_cursor + 4 > signed_end) return false;
    const uint32_t digests_length = le32(value, signed_cursor);
    signed_cursor += 4;
    if (digests_length > signed_end - signed_cursor) return false;
    signed_cursor += digests_length;
    if (signed_cursor + 4 > signed_end) return false;
    const uint32_t certificates_length = le32(value, signed_cursor);
    signed_cursor += 4;
    if (certificates_length == 0 || certificates_length > signed_end - signed_cursor) {
        return false;
    }
    size_t cert_cursor = signed_cursor;
    const size_t cert_end = signed_cursor + certificates_length;
    if (cert_cursor + 4 > cert_end) return false;
    const uint32_t cert_length = le32(value, cert_cursor);
    cert_cursor += 4;
    if (cert_length == 0 || cert_length > cert_end - cert_cursor) return false;
    certificate->assign(value.begin() + static_cast<std::ptrdiff_t>(cert_cursor),
                        value.begin() + static_cast<std::ptrdiff_t>(cert_cursor + cert_length));
    return true;
}

} // namespace

ApkSignatureResult read_apk_signer(const std::string &apk_path) {
    ApkSignatureResult result;
    std::ifstream file(apk_path, std::ios::binary | std::ios::ate);
    if (!file) {
        result.error = "signature_open";
        return result;
    }
    const uint64_t file_size = static_cast<uint64_t>(file.tellg());
    if (file_size < 22 + 16 + 24) {
        result.error = "signature_size";
        return result;
    }
    const size_t tail_size =
        static_cast<size_t>(std::min<uint64_t>(file_size, 22 + 65535));
    std::vector<uint8_t> tail;
    if (!read_at(file, file_size - tail_size, tail_size, &tail)) {
        result.error = "signature_eocd_read";
        return result;
    }
    size_t eocd = tail.size();
    for (size_t i = tail.size() - 22 + 1; i-- > 0;) {
        if (le32(tail, i) == kEocdSignature) {
            const uint16_t comment = le16(tail, i + 20);
            if (i + 22ull + comment == tail.size()) {
                eocd = i;
                break;
            }
        }
    }
    if (eocd == tail.size()) {
        result.error = "signature_eocd";
        return result;
    }
    const uint64_t central_offset = le32(tail, eocd + 16);
    if (central_offset < 24 || central_offset > file_size) {
        result.error = "signature_central";
        return result;
    }
    std::vector<uint8_t> footer;
    if (!read_at(file, central_offset - 24, 24, &footer)) {
        result.error = "signature_footer";
        return result;
    }
    if (std::memcmp(footer.data() + 8, kApkSigMagic, sizeof(kApkSigMagic)) != 0) {
        result.error = "signature_block_missing";
        return result;
    }
    const uint64_t block_size = le64(footer, 0);
    const uint64_t total = block_size + 8;
    if (total > kMaxSigningBlock || total > central_offset) {
        result.error = "signature_block_size";
        return result;
    }
    const uint64_t block_start = central_offset - total;
    std::vector<uint8_t> block;
    if (!read_at(file, block_start, static_cast<size_t>(total), &block)) {
        result.error = "signature_block_read";
        return result;
    }
    if (le64(block, 0) != block_size) {
        result.error = "signature_block_header";
        return result;
    }
    size_t at = 8;
    const size_t pairs_end = block.size() - 24;
    std::vector<uint8_t> certificate;
    bool found = false;
    while (at + 8 <= pairs_end) {
        const uint64_t pair_length = le64(block, at);
        if (pair_length < 4 || pair_length > pairs_end - at - 8) {
            result.error = "signature_pair";
            return result;
        }
        const uint32_t id = le32(block, at + 8);
        const size_t value_at = at + 12;
        const size_t value_length = static_cast<size_t>(pair_length - 4);
        if ((id == kV2Id || id == kV3Id) && value_length <= block.size() - value_at) {
            std::vector<uint8_t> value(block.begin() + static_cast<std::ptrdiff_t>(value_at),
                                       block.begin() +
                                           static_cast<std::ptrdiff_t>(value_at + value_length));
            if (first_certificate(value, &certificate)) {
                found = true;
                break;
            }
        }
        at += 8ull + pair_length;
    }
    if (!found) {
        result.error = "signature_certificate";
        return result;
    }
    result.signer_sha256 = shared::Sha256::hex(
        std::string(reinterpret_cast<const char *>(certificate.data()), certificate.size()));
    result.ok = true;
    return result;
}

} // namespace bybus::service
