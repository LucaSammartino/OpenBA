#pragma once
// ============================================================================
// src/ext/restricted_foods/restricted_foods.h -- ext.restricted_foods (class C, default OFF; see CLASS C)
// ============================================================================
// NOT RETAIL. docs/extensions/handins/RESTRICTEDFOODS.md. Chosen edible materials stop being EATEN by citizens
// while
// staying Edible for everything else (storage by category, markets, recipes, trade). The motivating case: raw
// Wheat is directly edible, so citizens eat it from the barn instead of the Bread a mod's bakery makes from
// it.
//
// WHERE. Retail decides "may a citizen eat this" in exactly two places, both a test of RawMaterialDescription
// `_flags` bit 5 (Edible), and every eating decision goes through one of them:
//   * CitizenComp_Entity_impl_ef40 (retail 0x14008ef40), category 0: the storage item picker the eating
//   action
//     uses to take an item out of a storage (citizencomponent.cpp, two passes over the same test);
//   * ResidenceComponent::GetFoodCount (retail 0x14012a9d0): the Edible count of a home's storage, which
//   gates
//     "does my home have food" (the go-eat decision, the starving icon, the heat of the home's food check).
// With the row active for the game, a blocked material counts as NOT Edible in those two tests and nowhere
// else.
//
// RULES. The town's list, one rule per entry (the text form accepts entries separated by `;`, `,` or a
// newline):
//     Wheat             block one material by its internal name (RawMaterialDescription._name)
//     Template\RawMaterialWheat.rsc:rawmaterial
//                       the same, written as the resource path (the name is cut from it)
//     group:Grain       block every Edible material in that food group: Fruit, Vegetable, Protein, Grain, or
//                       a 0x.. bit mask
//     all               block every Edible material
//     allow:Fish        exempt a material (always wins over the blocking rules)
//     Wheat=off         a trailing =off disables that entry
//     blocking=off      disables the whole list
// Names compare case-insensitively. A `hotkey=` entry is accepted and ignored: a hotkey is not part of this
// extension. With no rules the row blocks nothing, so switching it on alone changes nothing.
//
// IN GAME. The Foods page of the extension window (src/ext/restricted_foods/toolbar_page.cpp, hosted by
// src/ext/toolbar/: one toolbar button, one tab per active extension) lists the edible materials of the
// running
// town with a check box each; a checked food is not eaten from the moment it is checked, with no Apply.
//
// CLASS C. A blocked food is eaten later, from another source, or not at all, so a game under rules diverges
// from retail, and the list is per-town state a reload must restore: DESIGN.md "Which class, decided by one
// test" makes the row class C. The list lives in the save's tail record (save_state.cpp, wire format v1, by
// name), never in settings.cfg. The lock is on the row's activation (turned on before the town starts); the
// page edits the list live (DESIGN.md "What the lock covers").
//
// SELECTION (DESIGN.md hook rules 3 and 4): BeginFrame reads the row once per frame; the two seams test
// `g_on`
// first, so with the row off each pays one load and a not-taken branch. The verdict per material is cached by
// description pointer and dropped whenever the lock epoch changes (a new game, a load, a mod reload).
//
// The parser and the matching are standard C++ only: tests/unit/ext_restricted_foods_test.cpp links
// restricted_foods.cpp
// on its own.
// ============================================================================
#include <cstdint>
#include <string>
#include <vector>

namespace Ext { namespace SaveTail { struct StateCodec; } }

namespace Ext {
namespace RestrictedFoods {

constexpr uint32_t kEdible = 0x20u;                       // RawMaterialDescription._flags bit 5
// The four food-group bits (the ToolKit's RawMaterialFlags: Fruit 0, Vegetable 1, Protein 2, Grain 4).
constexpr uint32_t kFruit = 0x1u, kVegetable = 0x2u, kProtein = 0x4u, kGrain = 0x10u;

struct RuleSet {
    bool                     disabled = false;           // `blocking=off`
    bool                     all = false;                // `all`
    uint32_t                 groups = 0;                 // OR of the group bits named by `group:`
    std::vector<std::string> block;                      // lower-case internal names
    std::vector<std::string> allow;                      // lower-case internal names
    // entries that could not be read (unknown group, empty name)
    int                      malformed = 0;
};

// "Template\RawMaterialWheat.rsc:rawmaterial" -> "wheat"; "Wheat" -> "wheat"; case folded, never empty for a
// non-empty input.
std::string NameFromEntry(const std::string& entry);

// The rules text (the key's value, or a pasted file) -> a RuleSet. Never fails; what cannot be read is
// counted.
RuleSet ParseRules(const std::string& text);

// True when a citizen may NOT eat a material with these `flags` and this internal name under `rules`.
// A material that is not Edible is never blocked (nothing to take away); `allow:` beats every blocking rule.
bool Blocks(const RuleSet& rules, uint32_t flags, const std::string& nameLower);

// The rules as text (the inverse of ParseRules for everything ParseRules keeps; comments and `=off` entries
// are dropped). Empty for an empty set; `blocking=off` is written first when the list is disabled.
std::string FormatRules(const RuleSet& rules);

// Edit `rules` so that a material (internal name, flags) is blocked (`blocked` true) or not. A plain name
// entry is added
// or removed; when a group or `all` rule decides the other way, an `allow:` entry is added instead. Checking
// a food
// clears `disabled` (the player just asked for something to happen). Returns whether `rules` changed.
bool SetBlocked(RuleSet& rules, uint32_t flags, const std::string& nameLower, bool blocked);

// ---- run time (the two game seams)
// ----------------------------------------------------------------------------
// this frame: ext.restricted_foods (the game's locked value) and a rule set that can block something
extern bool g_on;

// Set the rules from text (the grammar above). Parsed at the next BeginFrame; the unit tests use it, the game
// goes through
// ApplyRules and the save codec.
void SetRulesText(const std::string& text);

// The live rules (what the seams use). The Foods page reads and edits through these.
const RuleSet& CurrentRules();
// Replace the live rules at once (the window's OK): the verdict cache is dropped and `g_on` follows at the
// next frame.
void ApplyRules(const RuleSet& rules);
// True when a food with these flags and internal name is blocked under the live rules (what the window
// shows).
bool IsBlockedNow(uint32_t flags, const std::string& nameLower);

// GameUpdateLoop::Update, once per frame beside SpreadWarmth::BeginFrame.
void BeginFrame();

// The seams' body: true = treat this Edible material as not edible. `desc` is the RawMaterialDescription
// (cache key),
// `flags` its `_flags`, `name` its `_name` string data (may be null: then only `all` and `group:` can match).
bool BlockedSlow(const void* desc, uint32_t flags, const char* name);
inline bool Blocked(const void* desc, uint32_t flags, const char* name) {
    return g_on && BlockedSlow(desc, flags, name);
}

struct Stats {
    uint64_t checks = 0;        // verdicts computed (cache misses)
    uint64_t blocked = 0;       // of those, materials blocked
};
Stats GetStats();

// The save-tail codec for this extension's per-town rules (save_state.cpp; registered in state_codecs.cpp).
const SaveTail::StateCodec& Codec();

}  // namespace RestrictedFoods
}  // namespace Ext
