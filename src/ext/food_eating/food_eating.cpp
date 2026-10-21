// src/ext/food_eating/food_eating.cpp -- NOT RETAIL. ext.food_eating (class B); see the header and
// docs/extensions/handins/FOODEATING.md.
#include "ext/food_eating/food_eating.h"
#include "ext/registry.h"   // Ext::Ext().food_eating (class C: the game's locked value), Ext::LockEpoch

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>

namespace Ext {
namespace FoodEating {

bool g_on = false;

namespace {

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string Trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

bool EndsWithNoCase(const std::string& s, const char* suffix) {
    const size_t n = std::char_traits<char>::length(suffix);
    if (s.size() < n) return false;
    return Lower(s.substr(s.size() - n)) == suffix;
}

// A food-group word or a number -> its bit(s); 0 when unknown.
uint32_t GroupBits(const std::string& wordIn) {
    const std::string w = Lower(Trim(wordIn));
    if (w == "fruit") return kFruit;
    if (w == "vegetable") return kVegetable;
    if (w == "protein") return kProtein;
    if (w == "grain") return kGrain;
    if (!w.empty() && (std::isdigit(static_cast<unsigned char>(w[0])))) {
        char* end = nullptr;
        const uint64_t v = std::strtoull(w.c_str(), &end, 0);
        if (end != nullptr && *end == 0) return static_cast<uint32_t>(v) & 0x1Fu;   // the five diet bits only
    }
    return 0;
}

// The pieces of one `;` / `,` / newline separated list.
std::vector<std::string> Split(const std::string& text) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : text) {
        if (c == ';' || c == ',' || c == '\n' || c == '\r') {
            out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    out.push_back(cur);
    return out;
}

}  // namespace

std::string NameFromEntry(const std::string& entryIn) {
    std::string e = Trim(entryIn);
    // The resource-path form: cut the directory, the ".rsc:rawmaterial" tail and the "RawMaterial" prefix.
    size_t slash = e.find_last_of("\\/");
    if (slash != std::string::npos) e = e.substr(slash + 1);
    size_t dot = e.find('.');
    if (dot != std::string::npos) e = e.substr(0, dot);
    size_t colon = e.find(':');
    if (colon != std::string::npos) e = e.substr(0, colon);
    if (e.size() > 11 && Lower(e.substr(0, 11)) == "rawmaterial") e = e.substr(11);
    return Lower(Trim(e));
}

RuleSet ParseRules(const std::string& text) {
    RuleSet r;
    for (std::string piece : Split(text)) {
        const size_t hash = piece.find('#');
        if (hash != std::string::npos) piece.erase(hash);                 // a trailing comment
        piece = Trim(piece);
        if (piece.empty()) continue;

        // `blocking=on|off` (the master switch) and `hotkey=..` (accepted and ignored) entries.
        const size_t eq = piece.find('=');
        if (eq != std::string::npos) {
            const std::string k = Lower(Trim(piece.substr(0, eq)));
            const std::string v = Lower(Trim(piece.substr(eq + 1)));
            if (k == "blocking" || k == "start") {
                if (v == "off" || v == "0" || v == "false" || v == "no") r.disabled = true;
                else if (v == "on" || v == "1" || v == "true" || v == "yes") r.disabled = false;
                else ++r.malformed;
                continue;
            }
            // the live hotkey is not part of this extension
            if (k == "hotkey") continue;
        }

        // A trailing `=off` disables the entry.
        if (EndsWithNoCase(piece, "=off")) continue;

        const std::string lower = Lower(piece);
        if (lower == "all") { r.all = true; continue; }
        if (lower.compare(0, 6, "group:") == 0) {
            const uint32_t bits = GroupBits(piece.substr(6));
            if (bits == 0) {
                ++r.malformed;
            } else {
                r.groups |= bits;
            }
            continue;
        }
        if (lower.compare(0, 6, "allow:") == 0) {
            const std::string n = NameFromEntry(piece.substr(6));
            if (n.empty()) {
                ++r.malformed;
            } else {
                r.allow.push_back(n);
            }
            continue;
        }
        const std::string n = NameFromEntry(piece);
        if (n.empty()) {
            ++r.malformed;
        } else {
            r.block.push_back(n);
        }
    }
    return r;
}

bool Blocks(const RuleSet& rules, uint32_t flags, const std::string& nameLower) {
    if (rules.disabled) return false;
    if ((flags & kEdible) == 0) return false;                            // not edible: nothing to take away
    if (!nameLower.empty() &&
        std::find(rules.allow.begin(), rules.allow.end(), nameLower) != rules.allow.end())
        return false;                                                    // allow: beats every blocking rule
    if (rules.all) return true;
    if ((rules.groups & flags) != 0) return true;
    if (!nameLower.empty() &&
        std::find(rules.block.begin(), rules.block.end(), nameLower) != rules.block.end())
        return true;
    return false;
}

std::string FormatRules(const RuleSet& r) {
    std::string out;
    auto add = [&out](const std::string& item) {
        if (!out.empty()) out += "; ";
        out += item;
    };
    if (r.disabled) add("blocking=off");
    if (r.all) add("all");
    uint32_t left = r.groups;
    static const struct { uint32_t bit; const char* name; } kGroups[] = {
        {kFruit, "Fruit"}, {kVegetable, "Vegetable"}, {kProtein, "Protein"}, {kGrain, "Grain"}};
    for (const auto& g : kGroups) {
        if ((left & g.bit) != 0) { add(std::string("group:") + g.name); left &= ~g.bit; }
    }
    if (left != 0) {
        char hex[16];
        std::snprintf(hex, sizeof hex, "0x%X", left);
        add(std::string("group:") + hex);
    }
    for (const std::string& n : r.block) add(n);
    for (const std::string& n : r.allow) add("allow:" + n);
    return out;
}

bool SetBlocked(RuleSet& rules, uint32_t flags, const std::string& nameLower, bool blocked) {
    if (nameLower.empty()) return false;
    bool changed = false;
    auto erase = [&changed](std::vector<std::string>& v, const std::string& n) {
        const size_t before = v.size();
        v.erase(std::remove(v.begin(), v.end(), n), v.end());
        if (v.size() != before) changed = true;
    };
    if (blocked) {
        if (rules.disabled) { rules.disabled = false; changed = true; }
        erase(rules.allow, nameLower);
        if (!Blocks(rules, flags, nameLower)) { rules.block.push_back(nameLower); changed = true; }
    } else {
        erase(rules.block, nameLower);
        if (Blocks(rules, flags, nameLower)) { rules.allow.push_back(nameLower); changed = true; }
    }
    return changed;
}

namespace {

std::string                          s_text;            // the settings value, as given
RuleSet                              s_rules;
bool                                 s_dirty = true;    // s_text changed: parse again
uint32_t                             s_epoch = ~0u;     // the lock epoch the cache belongs to
std::unordered_map<const void*, bool> s_cache;
Stats                                s_stats;

bool CanBlock(const RuleSet& r) {
    return !r.disabled && (r.all || r.groups != 0 || !r.block.empty());
}

}  // namespace

void SetRulesText(const std::string& text) {
    s_text = text;
    s_dirty = true;
}

const RuleSet& CurrentRules() {
    // a rules text set since the last BeginFrame: the window can open before one
    if (s_dirty) {
        s_rules = ParseRules(s_text);
        s_dirty = false;
        s_cache.clear();
    }
    return s_rules;
}

void ApplyRules(const RuleSet& rules) {
    s_rules = rules;
    s_text = FormatRules(rules);
    s_dirty = false;
    s_cache.clear();
}

bool IsBlockedNow(uint32_t flags, const std::string& nameLower) {
    return Blocks(CurrentRules(), flags, nameLower);
}

void BeginFrame() {
    if (s_dirty) {
        s_rules = ParseRules(s_text);
        s_dirty = false;
        s_cache.clear();
    }
    const uint32_t epoch = LockEpoch();
    // a new game, a load or a reload: descriptions may be new
    if (epoch != s_epoch) {
        s_epoch = epoch;
        s_cache.clear();
    }
    g_on = Ext::Ext().food_eating && CanBlock(s_rules);   // class C: the game's locked value
}

bool BlockedSlow(const void* desc, uint32_t flags, const char* name) {
    if ((flags & kEdible) == 0) return false;
    auto it = s_cache.find(desc);
    if (it != s_cache.end()) return it->second;
    ++s_stats.checks;
    const bool blocked = Blocks(s_rules, flags, name != nullptr ? Lower(name) : std::string());
    if (blocked) ++s_stats.blocked;
    s_cache.emplace(desc, blocked);
    return blocked;
}

Stats GetStats() { return s_stats; }

}  // namespace FoodEating
}  // namespace Ext
