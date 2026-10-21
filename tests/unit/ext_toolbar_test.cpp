// ============================================================================
// tests/unit/ext_toolbar_test.cpp -- the extension window's page list (src/ext/toolbar/toolbar.cpp)
// ============================================================================
// A page is offered only while its extension's row is active for the game, the host adds its button only when
// some page is
// offered, and the gate's command-line faithful profile never offers it. The page itself (UI code, exe only)
// is stubbed here.
// ============================================================================
#include "unit.h"
#include "ext/registry.h"
#include "ext/toolbar/toolbar.h"
#include "io/settings_file.h"

#include <cstring>
#include <string>

namespace {

using IO::SettingsFile::Entries;

bool EntriesLookup(void* ctx, const char* key, std::string& value) {
    const Entries* entries = static_cast<const Entries*>(ctx);
    for (const IO::SettingsFile::Entry& e : *entries)
        if (e.key == key) { value = e.value; return true; }
    return false;
}

Ext::State ResolveText(const char* text) {
    Entries entries = IO::SettingsFile::Parse(std::string(text));
    return Ext::Resolve(&EntriesLookup, &entries, nullptr, nullptr);
}

void StubOpen(UI::Element*) {}
bool StubChanged(UI::Element*, int32_t) { return false; }
void StubClose() {}

}  // namespace

// The page the exe defines in src/ext/food_eating/toolbar_page.cpp.
const Ext::Toolbar::Contribution& Ext::Toolbar::FoodEatingPage() {
    static const Contribution c = { "food_eating", "FoodEatingTab",
                                    "Dialog/FoodEatingPage.rsc:pageFoodEating",
                                    StubOpen, StubChanged, StubClose };
    return c;
}

UNIT_TEST(ext_toolbar_no_page_while_the_row_is_off) {
    Ext::Install(ResolveText(""), Ext::Profile::PerEntry, false);
    const Ext::Toolbar::Contribution* pages[4] = {};
    CHECK_EQ(Ext::Toolbar::ActivePages(pages, 4), 0);
    // no active contributor: no button, the toolbar is retail's
    CHECK(!Ext::Toolbar::Offered());
}

UNIT_TEST(ext_toolbar_a_page_is_offered_while_its_row_is_active) {
    Ext::Install(ResolveText("ext.food_eating=1\n"), Ext::Profile::PerEntry, false);
    const Ext::Toolbar::Contribution* pages[4] = {};
    CHECK_EQ(Ext::Toolbar::ActivePages(pages, 4), 1);
    CHECK(pages[0] != nullptr && std::strcmp(pages[0]->id, "food_eating") == 0);
    CHECK(Ext::Toolbar::Offered());
    CHECK_EQ(Ext::Toolbar::ActivePages(nullptr, 0), 1);    // the count alone, no output buffer
    int n = 0;
    CHECK(Ext::Toolbar::All(&n) != nullptr && n == 1);
    Ext::Install(ResolveText(""), Ext::Profile::PerEntry, false);
}

UNIT_TEST(ext_toolbar_the_faithful_gate_never_offers_the_button) {
    Ext::State s = ResolveText("ext.food_eating=1\n");
    Ext::Install(s, Ext::Profile::Faithful, true);         // /profile:faithful on the command line
    CHECK(Ext::GateProfileForced());
    CHECK(!Ext::Toolbar::Offered());
    Ext::Install(ResolveText(""), Ext::Profile::PerEntry, false);
}

UNIT_TEST(ext_toolbar_every_page_names_a_registered_class_c_or_b_row) {
    int n = 0;
    const Ext::Toolbar::Contribution* const* list = Ext::Toolbar::All(&n);
    for (int i = 0; i < n; ++i) {
        const Ext::Entry* e = Ext::FindEntry(list[i]->id);
        CHECK(e != nullptr);
        if (e) CHECK(e->tier == Ext::Tier::Extension);
        CHECK(list[i]->caption != nullptr && list[i]->page != nullptr && list[i]->open && list[i]->changed &&
              list[i]->close);
    }
}
