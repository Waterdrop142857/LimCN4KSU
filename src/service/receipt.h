// service/receipt.h
// Responsibility: read the runtime receipt written by the Zygisk companion
// (12-kernelsu-appsealing.md §4) without trusting its shape. Only counts, fixed
// state tokens and a protocol code cross the boundary; no game text.
#pragma once

#include <cstdint>
#include <string>

namespace bybus::service {

struct RuntimeReceipt {
    std::string generation;
    std::string hook_state;   // none | installed | rejected
    std::string font_state;   // none | ready | rejected
    std::string compat_state; // none | installed | rejected
    std::string code;         // protocol code
    uint64_t hits = 0;
    uint64_t misses = 0;
    bool installed = false;
    bool confirmed = false;
};

// Missing or malformed receipts yield an empty struct, never an error.
RuntimeReceipt read_receipt(const std::string &path);

} // namespace bybus::service
