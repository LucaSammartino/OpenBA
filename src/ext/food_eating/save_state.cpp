// ============================================================================
// src/ext/food_eating/save_state.cpp -- ext.food_eating's per-town state for the save tail (class C)
// ============================================================================
// NOT RETAIL. docs/extensions/handins/FOODEATING.md, DESIGN.md "Class C state: the contract".
//
// The state is the rules list (food_eating.h): block entries by internal material name, `group:` and `all`
// entries,
// `allow:` exemptions, and `blocking=off`. It is stored by NAME, never by index, because mods change the load
// order
// between sessions (and a name the game no longer has simply matches nothing).
//
// Wire format v1 (little-endian): u32 count, count x { u32 length, u8 entry[length] }. Each entry is one rule
// in the
// text grammar (`wheat`, `group:Grain`, `all`, `allow:fish`, `blocking=off`). A decode that finds a rule it
// cannot read
// refuses the payload (the save is damaged), the way a truncated payload does.
//
// Standard C++ only: tests/unit/ext_food_eating_test.cpp links this file with food_eating.cpp and
// savetail.cpp.
// ============================================================================

#include "ext/food_eating/food_eating.h"

#include "ext/savetail.h"

#include <string>
#include <vector>

namespace Ext {
namespace FoodEating {
namespace {

// "a; b;c" -> {"a", "b", "c"}: the rules text FormatRules writes, one entry at a time.
std::vector<std::string> SplitEntries(const std::string& text) {
    std::vector<std::string> out;
    std::string cur;
    auto flush = [&]() {
        size_t a = 0, b = cur.size();
        while (a < b && (cur[a] == ' ' || cur[a] == '\t')) ++a;
        while (b > a && (cur[b - 1] == ' ' || cur[b - 1] == '\t')) --b;
        if (b > a) out.push_back(cur.substr(a, b - a));
        cur.clear();
    };
    for (char c : text) {
        if (c == ';') {
            flush();
        } else {
            cur.push_back(c);
        }
    }
    flush();
    return out;
}

void Encode(std::vector<uint8_t>& payload) {
    const std::vector<std::string> entries = SplitEntries(FormatRules(CurrentRules()));
    SaveTail::Writer w(payload);
    w.U32(static_cast<uint32_t>(entries.size()));
    for (const std::string& e : entries) w.String(e);
}

bool Decode(const uint8_t* bytes, size_t size, int32_t savedVersion) {
    // Validate already refused a NEWER version; an older one would migrate here
    if (savedVersion != 1) return false;
    SaveTail::Reader r(bytes, size);
    uint32_t count = 0;
    if (!r.U32(count)) return false;
    std::string joined;
    for (uint32_t i = 0; i < count; ++i) {
        std::string entry;
        if (!r.String(entry)) return false;
        if (entry.find(';') != std::string::npos) return false;   // an entry is one rule
        if (!joined.empty()) joined += "; ";
        joined += entry;
    }
    if (!r.AtEnd()) return false;
    const RuleSet rules = ParseRules(joined);
    if (rules.malformed != 0) return false;
    ApplyRules(rules);
    return true;
}

void Reset() { ApplyRules(RuleSet()); }

}  // namespace

const SaveTail::StateCodec& Codec() {
    static const SaveTail::StateCodec c = { "food_eating", Encode, Decode, Reset };
    return c;
}

}  // namespace FoodEating
}  // namespace Ext
