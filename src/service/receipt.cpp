#include "service/receipt.h"

#include <string>

#include "service/paths.h"
#include "shared/fs.h"
#include "shared/json.h"

namespace bybus::service {
namespace {

std::string text_field(const shared::JsonPtr &root, const char *name) {
    const shared::JsonPtr value = root->opt(name);
    return value && value->is_string() ? value->as_string() : std::string();
}

uint64_t count_field(const shared::JsonPtr &root, const char *name) {
    const shared::JsonPtr value = root->opt(name);
    if (!value || !value->is_integral() || value->as_int() < 0) return 0;
    return static_cast<uint64_t>(value->as_int());
}

} // namespace

RuntimeReceipt read_receipt(const std::string &path) {
    RuntimeReceipt receipt;
    std::string body;
    if (shared::read_file(path, kMaxReceiptBytes, &body) != shared::Code::Ok) return receipt;
    const shared::ParseOutcome parsed = shared::parse(body);
    if (!parsed.ok || !parsed.value || !parsed.value->is_object()) return receipt;
    const shared::JsonPtr root = parsed.value;
    receipt.generation = text_field(root, "generation");
    receipt.hook_state = text_field(root, "hookState");
    receipt.font_state = text_field(root, "fontState");
    receipt.compat_state = text_field(root, "compatState");
    receipt.code = text_field(root, "code");
    receipt.hits = count_field(root, "hits");
    receipt.misses = count_field(root, "misses");
    receipt.installed = receipt.hook_state == "installed";
    const shared::JsonPtr confirmed = root->opt("confirmed");
    receipt.confirmed = confirmed && confirmed->is_bool() && confirmed->as_bool();
    return receipt;
}

} // namespace bybus::service
