// Host checks for the signature normalization used by every verified method
// lookup. The two spellings below are exactly the ones the reference runtime
// tolerates; a raw strcmp against either would miss the other.
#include <string>
#include <vector>

#include "harness.h"
#include "runtime/type_signature.h"

namespace {

using bybus::runtime::type_signature::normalize;
using bybus::runtime::type_signature::split;

const char *const kAddMaterialParameters =
    "UnityEngine.Material,TMPro.TMP_FontAsset,TMPro.MaterialReference[]&,"
    "System.Collections.Generic.Dictionary<System.Int32,System.Int32>";

} // namespace

BYBUS_TEST(type_signature_generic_arity_is_optional) {
    check(normalize("System.Collections.Generic.Dictionary`2<System.Int32,System.Int32>") ==
              normalize("System.Collections.Generic.Dictionary<System.Int32,System.Int32>"),
          "dictionary arity suffix must not change the signature");
}

BYBUS_TEST(type_signature_nested_separator_is_optional) {
    check(normalize("TMPro.TMP_Text/Axis") == normalize("TMPro.TMP_Text.Axis"),
          "nested type separator must not change the signature");
}

BYBUS_TEST(type_signature_split_two_parameters) {
    std::vector<std::string> parts;
    check(split("System.Int32,System.Int32", &parts), "two ints must split");
    check(parts.size() == 2, "two ints must produce two parameters");
}

BYBUS_TEST(type_signature_split_generic_comma_stays_inside) {
    std::vector<std::string> parts;
    check(split(kAddMaterialParameters, &parts), "AddMaterialReference list must split");
    check(parts.size() == 4, "AddMaterialReference has four parameters");
    if (parts.size() == 4) {
        check(parts[2] == "TMPro.MaterialReference[]&",
              "by-reference array parameter must keep its markers");
        check(parts[3] == "System.Collections.Generic.Dictionary<System.Int32,System.Int32>",
              "generic dictionary must stay one parameter");
    }
}

BYBUS_TEST(type_signature_split_rejects_malformed_lists) {
    std::vector<std::string> parts;
    check(!split("a,,b", &parts), "empty middle parameter must be rejected");
    check(!split("a,b,", &parts), "trailing comma must be rejected");
    check(!split("<a", &parts), "unbalanced angle bracket must be rejected");
    std::string seventeen;
    for (int index = 0; index < 17; ++index) {
        if (index != 0) seventeen.push_back(',');
        seventeen += "System.Int32";
    }
    check(!split(seventeen.c_str(), &parts), "17 parameters exceed the default limit");
}

BYBUS_TEST(type_signature_split_empty_list_is_valid) {
    std::vector<std::string> parts;
    check(split("", &parts), "an empty parameter list is valid");
    check(parts.empty(), "an empty parameter list has no parameter");
    check(split("", &parts, 0), "the empty list also fits a zero limit");
}
