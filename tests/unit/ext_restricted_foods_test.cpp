// ============================================================================
// tests/unit/ext_restricted_foods_test.cpp -- ext.restricted_foods
// ============================================================================
// src/ext/restricted_foods/restricted_foods.{h,cpp}, save_state.cpp without the game: the registry row (class C, off by
// default),
// the save-tail codec (round trip, empty state, damaged payload, the tail), the rules grammar (names,
// resource paths, groups, all, allow:, =off,
// blocking=off, comments, separators, malformed entries), the matching (a non-edible material is never
// blocked,
// allow: wins, group bits, case folding), and the run-time seam (inert with the row off or no rules).
// ============================================================================
#include "unit.h"
#include "ext/restricted_foods/restricted_foods.h"
#include "ext/registry.h"
#include "ext/savetail.h"
#include "io/settings_file.h"

#include <string>
#include <vector>

namespace {

using IO::SettingsFile::Entries;
using namespace Ext::RestrictedFoods;

bool EntriesLookup(void* ctx, const char* key, std::string& value) {
    const Entries* entries = static_cast<const Entries*>(ctx);
    for (const IO::SettingsFile::Entry& e : *entries) {
        if (e.key == key) { value = e.value; return true; }
    }
    return false;
}

Ext::State ResolveText(const char* text) {
    Entries entries = IO::SettingsFile::Parse(std::string(text));
    return Ext::Resolve(&EntriesLookup, &entries, nullptr, nullptr);
}

constexpr uint32_t kWheatFlags = kEdible | kGrain;        // raw wheat: Edible, Grain
constexpr uint32_t kFishFlags  = kEdible | kProtein;
constexpr uint32_t kAppleFlags = kEdible | kFruit;
constexpr uint32_t kWoodFlags  = 0x40;                    // Fuel only: not edible

}  // namespace

UNIT_TEST(ext_restricted_foods_registry_row) {
    const Ext::Entry* e = Ext::FindEntry("restricted_foods");
    CHECK(e != nullptr);
    if (!e) return;
    CHECK(e->tier == Ext::Tier::Extension);
    // per-town state: the checked list is recorded in the save
    CHECK(e->cls == Ext::Class::C);
    CHECK(!e->defaultOn);
    CHECK(e->formatVersion == 1);
    CHECK(Ext::SettingsKey(*e) == "ext.restricted_foods");
    CHECK(!ResolveText("").ext.restricted_foods);                          // absent key = the default (off)
    CHECK(ResolveText("ext.restricted_foods=1\n").ext.restricted_foods);
    CHECK(!ResolveText("profile=faithful\n").ext.restricted_foods);       // the faithful profile turns it off
    CHECK(Ext::SaveTail::FindCodec("restricted_foods") != nullptr);        // class C: it has a state codec
}

UNIT_TEST(ext_restricted_foods_state_codec_round_trip) {
    const Ext::SaveTail::StateCodec* c = Ext::SaveTail::FindCodec("restricted_foods");
    CHECK(c != nullptr);
    if (!c) return;
    ApplyRules(ParseRules("Wheat; group:Protein; allow:Fish; blocking=off"));
    std::vector<uint8_t> payload;
    c->encode(payload);
    CHECK(!payload.empty());
    c->reset();                                                        // a new game: nothing carried over
    CHECK(CurrentRules().block.empty() && !CurrentRules().disabled && CurrentRules().groups == 0);
    CHECK(c->decode(payload.data(), payload.size(), 1));
    const RuleSet& r = CurrentRules();
    CHECK(r.disabled && r.groups == kProtein);
    CHECK(r.block.size() == 1 && r.block[0] == "wheat");
    CHECK(r.allow.size() == 1 && r.allow[0] == "fish");
    std::vector<uint8_t> again;
    c->encode(again);
    CHECK(again == payload);                                           // stable: encode(decode(x)) == x
    c->reset();
}

UNIT_TEST(ext_restricted_foods_state_codec_empty_state_is_still_a_valid_record) {
    const Ext::SaveTail::StateCodec* c = Ext::SaveTail::FindCodec("restricted_foods");
    if (!c) { CHECK(false); return; }
    c->reset();
    std::vector<uint8_t> payload;
    c->encode(payload);
    CHECK_EQ(payload.size(), 4u);                                      // count 0
    CHECK(c->decode(payload.data(), payload.size(), 1));
    CHECK(CurrentRules().block.empty());
}

UNIT_TEST(ext_restricted_foods_state_codec_refuses_a_damaged_payload) {
    const Ext::SaveTail::StateCodec* c = Ext::SaveTail::FindCodec("restricted_foods");
    if (!c) { CHECK(false); return; }
    ApplyRules(ParseRules("wheat; corn"));
    std::vector<uint8_t> payload;
    c->encode(payload);
    c->reset();
    CHECK(!c->decode(payload.data(), payload.size() - 1, 1));         // cut short
    std::vector<uint8_t> extra = payload;
    extra.push_back(0);
    CHECK(!c->decode(extra.data(), extra.size(), 1));                  // bytes left over
    CHECK(!c->decode(payload.data(), payload.size(), 2));              // a layout this build does not know
    CHECK(!c->decode(nullptr, 0, 1));                                  // no count at all
    std::vector<uint8_t> bad;                                          // one entry, a group name nobody has
    Ext::SaveTail::Writer w(bad);
    w.U32(1);
    w.String("group:Nonsense");
    CHECK(!c->decode(bad.data(), bad.size(), 1));
    c->reset();
}

UNIT_TEST(ext_restricted_foods_state_travels_in_the_tail) {
    // The registry's ACTIVE class C row carries the codec's payload in its own record; the lock gives it back
    // with the save.
    ApplyRules(ParseRules("wheat"));
    Ext::State s = ResolveText("ext.restricted_foods=1\n");
    Ext::Install(s, Ext::Profile::PerEntry, false);
    Ext::LockForGame(&s);
    std::vector<Ext::SaveTail::Record> recs = Ext::SaveTail::ActiveRecords();
    bool found = false;
    for (const Ext::SaveTail::Record& r : recs)
        if (r.id == "restricted_foods") { found = true; CHECK(!r.payload.empty() && r.formatVersion == 1); }
    CHECK(found);
    const std::vector<uint8_t> tail = Ext::SaveTail::Encode(recs);
    ApplyRules(RuleSet());
    std::vector<Ext::SaveTail::Record> back;
    CHECK(Ext::SaveTail::Decode(tail.data(), tail.size(), back) == Ext::SaveTail::Status::Ok);
    std::string offending;
    CHECK(Ext::SaveTail::Validate(back, &offending) == Ext::SaveTail::Status::Ok);
    CHECK(Ext::SaveTail::ApplyState(back, &offending));
    // the town's list came back with the save
    CHECK(IsBlockedNow(kWheatFlags, "wheat"));
    Ext::UnlockForGame(&s);
    Ext::SaveTail::ResetAllState();
    CHECK(!IsBlockedNow(kWheatFlags, "wheat"));                        // and went with the game
    Ext::Install(ResolveText(""), Ext::Profile::PerEntry, false);
}

UNIT_TEST(ext_restricted_foods_names_from_entries) {
    CHECK(NameFromEntry("Wheat") == "wheat");
    CHECK(NameFromEntry("  RabbitMeat ") == "rabbitmeat");
    CHECK(NameFromEntry("Template\\RawMaterialWheat.rsc:rawmaterial") == "wheat");
    CHECK(NameFromEntry("Template/RawMaterialRabbitMeat.rsc:rawmaterial") == "rabbitmeat");
    // the prefix compares case-insensitively
    CHECK(NameFromEntry("template\\rawmaterialfish.rsc:rawmaterial") == "fish");
    // nothing left after the prefix: kept whole
    CHECK(NameFromEntry("RawMaterial") == "rawmaterial");
    CHECK(NameFromEntry("").empty());
}

UNIT_TEST(ext_restricted_foods_parses_rules) {
    RuleSet r = ParseRules(
        "Wheat; Template\\RawMaterialCorn.rsc:rawmaterial, group:Fruit\nallow:Apple # keep apples\n");
    CHECK(!r.disabled && !r.all);
    CHECK(r.block.size() == 2 && r.block[0] == "wheat" && r.block[1] == "corn");
    CHECK(r.groups == kFruit);
    CHECK(r.allow.size() == 1 && r.allow[0] == "apple");
    CHECK(r.malformed == 0);

    RuleSet all = ParseRules("all");
    CHECK(all.all);
    CHECK(ParseRules("ALL").all);

    RuleSet off = ParseRules("Wheat=off; Fish");                      // a trailing =off disables that entry
    CHECK(off.block.size() == 1 && off.block[0] == "fish");

    CHECK(ParseRules("blocking=off; Wheat").disabled);                // the master switch
    CHECK(!ParseRules("blocking=on; Wheat").disabled);
    // the live hotkey is ignored, not an entry
    CHECK(ParseRules("hotkey=0x2D; Wheat").block.size() == 1);
    CHECK(ParseRules("hotkey=0x2D").malformed == 0);

    RuleSet g = ParseRules("group:Grain; group:0x2; group:Protein");
    CHECK(g.groups == (kGrain | kVegetable | kProtein));
    // unknown group, empty names: counted, never thrown
    RuleSet bad = ParseRules("group:Dairy; allow:; group:");
    CHECK(bad.malformed == 3 && bad.groups == 0 && bad.allow.empty());
    CHECK(ParseRules("").block.empty() && ParseRules(" ;; ,\n").block.empty());
}

UNIT_TEST(ext_restricted_foods_blocks) {
    RuleSet r = ParseRules("Wheat; group:Protein; allow:Fish");
    CHECK(Blocks(r, kWheatFlags, "wheat"));                           // by name
    // a fruit is not named or in a blocked group
    CHECK(!Blocks(r, kAppleFlags, "apple"));
    CHECK(Blocks(r, kProtein | kEdible, "venison"));                  // by group
    CHECK(!Blocks(r, kFishFlags, "fish"));                            // allow: beats the group rule
    // not Edible: never blocked, even if named
    CHECK(!Blocks(r, kWoodFlags | kGrain, "wheat"));

    RuleSet all = ParseRules("all; allow:Apple");
    CHECK(Blocks(all, kWheatFlags, "wheat") && Blocks(all, kFishFlags, "fish"));
    CHECK(!Blocks(all, kAppleFlags, "apple"));                        // allow: beats all
    CHECK(!Blocks(all, kWoodFlags, "firewood"));

    CHECK(!Blocks(ParseRules("blocking=off; all"), kWheatFlags, "wheat"));   // the master switch
    // no name: all / group still match
    CHECK(Blocks(ParseRules("all"), kWheatFlags, ""));
    // no name: a named rule cannot match
    CHECK(!Blocks(ParseRules("Wheat"), kWheatFlags, ""));
    CHECK(!Blocks(RuleSet(), kWheatFlags, "wheat"));                         // no rules: nothing blocked
}

UNIT_TEST(ext_restricted_foods_seam_is_inert_by_default) {
    CHECK(!g_on);
    const Stats before = GetStats();
    // g_on false: the slow path is never reached
    CHECK(!Blocked(nullptr, kWheatFlags, "wheat"));
    const Stats after = GetStats();
    CHECK(after.checks == before.checks && after.blocked == before.blocked);
}

UNIT_TEST(ext_restricted_foods_seam_caches_verdicts) {
    SetRulesText("Wheat; group:Protein");
    // parses the rules; the row is off in the unit state, so g_on stays false
    BeginFrame();
    CHECK(!g_on);
    const Stats before = GetStats();
    // stand-ins for three description pointers
    int wheat = 0, apple = 0, wood = 0;
    CHECK(BlockedSlow(&wheat, kWheatFlags, "Wheat"));                 // named (case folded)
    CHECK(BlockedSlow(&wheat, kWheatFlags, "Wheat"));                 // second call: the cached verdict
    CHECK(!BlockedSlow(&apple, kAppleFlags, "Apple"));
    // not Edible: answered without a verdict
    CHECK(!BlockedSlow(&wood, kWoodFlags, "Firewood"));
    const Stats after = GetStats();
    CHECK(after.checks - before.checks == 2);                         // wheat once, apple once
    CHECK(after.blocked - before.blocked == 1);
    SetRulesText("");
    BeginFrame();                                                     // new rules: the cache is dropped
    CHECK(!BlockedSlow(&wheat, kWheatFlags, "Wheat"));
}

UNIT_TEST(ext_restricted_foods_formats_rules) {
    CHECK(FormatRules(RuleSet()).empty());
    CHECK(FormatRules(ParseRules("Wheat")) == "wheat");
    CHECK(FormatRules(ParseRules("Wheat; Corn; group:Grain; allow:Rye")) ==
          "group:Grain; wheat; corn; allow:rye");
    CHECK(FormatRules(ParseRules("all; blocking=off")) == "blocking=off; all");
    CHECK(FormatRules(ParseRules("group:0x2; group:Fruit")) == "group:Fruit; group:Vegetable");
    // bits no name covers keep a hex number
    CHECK(FormatRules(ParseRules("group:0x8")) == "group:0x8");
    // The text parses back to the same set.
    const std::string text = "group:Protein; wheat; corn; allow:fish";
    RuleSet back = ParseRules(FormatRules(ParseRules(text)));
    CHECK(back.groups == kProtein && back.block.size() == 2 && back.allow.size() == 1 &&
          back.allow[0] == "fish");
}

UNIT_TEST(ext_restricted_foods_set_blocked_edits_the_rules) {
    RuleSet r;
    CHECK(SetBlocked(r, kWheatFlags, "wheat", true));                  // check a food: a name entry
    CHECK(r.block.size() == 1 && Blocks(r, kWheatFlags, "wheat"));
    CHECK(!SetBlocked(r, kWheatFlags, "wheat", true));                 // already blocked: nothing changes
    CHECK(SetBlocked(r, kWheatFlags, "wheat", false));                 // uncheck: the entry goes
    CHECK(r.block.empty() && r.allow.empty() && !Blocks(r, kWheatFlags, "wheat"));
    CHECK(!SetBlocked(r, kWheatFlags, "wheat", false));                // already free

    RuleSet g = ParseRules("group:Grain");                             // a group rule blocks wheat
    CHECK(Blocks(g, kWheatFlags, "wheat"));
    // unchecking one grain: an allow: entry, not a lost rule
    CHECK(SetBlocked(g, kWheatFlags, "wheat", false));
    CHECK(g.allow.size() == 1 && g.allow[0] == "wheat" && g.groups == kGrain);
    CHECK(!Blocks(g, kWheatFlags, "wheat") && Blocks(g, kGrain | kEdible, "barley"));
    // check it again: the allow: goes, the group decides
    CHECK(SetBlocked(g, kWheatFlags, "wheat", true));
    CHECK(g.allow.empty() && g.block.empty() && Blocks(g, kWheatFlags, "wheat"));

    RuleSet off = ParseRules("blocking=off; wheat");
    CHECK(!Blocks(off, kWheatFlags, "wheat"));
    // checking something clears the master switch
    CHECK(SetBlocked(off, kFishFlags, "fish", true));
    CHECK(!off.disabled && Blocks(off, kFishFlags, "fish"));

    RuleSet none;
    CHECK(!SetBlocked(none, kWheatFlags, "", true));                   // no name: nothing to key on
}

UNIT_TEST(ext_restricted_foods_live_rules_round_trip) {
    SetRulesText("wheat");
    // CurrentRules parses a pending text without a frame
    CHECK(IsBlockedNow(kWheatFlags, "wheat"));
    RuleSet r = CurrentRules();
    CHECK(SetBlocked(r, kFishFlags, "fish", true));
    ApplyRules(r);
    CHECK(IsBlockedNow(kFishFlags, "fish") && IsBlockedNow(kWheatFlags, "wheat"));
    ApplyRules(RuleSet());
    CHECK(!IsBlockedNow(kFishFlags, "fish") && !IsBlockedNow(kWheatFlags, "wheat"));
    SetRulesText("");
}
