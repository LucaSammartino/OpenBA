// ============================================================================
// src/ext/food_eating/toolbar_page.cpp -- ext.food_eating's page in the extension window (NOT RETAIL)
// ============================================================================
// docs/extensions/handins/FOODEATING.md, src/ext/toolbar/toolbar.h. One cell per edible food of the running
// town with a
// check box; a checked food is not eaten by citizens, from the moment it is checked (live: no Apply). The
// list is laid out
// like the Town Hall's Inventory tab (TownHall.rsc:inventoryTable / storageItem): a table of four columns of
// small cells,
// each a check box and a compact label (20 px icon, small font), so a modded game's hundreds of foods are a
// short scroll.
// The page sizes the table and clones foodCell into it, as TownHallUI::InitInventory does.
//
// The page's objects are ours (src/ext/ui/Dialog/FoodEatingPage.rsc); the texts are in
// ExtensionsStringTable.rsc (the
// FoodEating* keys). It looks up, by name: textHint, tableFoodEating, buttonFoodEatingRow's one child (the
// filter
// button, whose label it swaps), and in each cell checkFoodEating and labelFoodEating.
//
// The checked list is the town's own state (class C): it lives in this extension's rules (food_eating.h),
// which the save
// tail carries (save_state.cpp). The page reads and writes it through ApplyRules and IsBlockedNow.
// ============================================================================

// The runtime type headers first (moddialog.cpp's note: Volume.h's Rectangle).
#include "ui/TextElement.h"
#include "ui/ImageElement.h"
#include "ui/ButtonElement.h"
#include "ui/CheckElement.h"
#include "ui/ElementDescription.h"
#include "ui/Element.h"
#include "ui/TableElement.h"
#include "ui/LabelElement.h"
#include "ui/Sprite.h"
#include "ui/SpriteSheet.h"
#include "ui/StringTable.h"
#include "ui/String.h"
#include "ui/Dialog.h"
#include "object/External.h"
#include "object/Internal.h"
#include "object/TypeInfo.h"
#include "system/String.h"
#include "system/Filename.h"
#include "Game/GameUpdateLoop.h"
#include "Component/RawMaterialDescription.h"
#include "Entity/CitizenBehavior.h"
#include "Component/ComponentDescription.h"
#include "Component/Description.h"

#include "ext/food_eating/food_eating.h"
#include "ext/toolbar/toolbar.h"
#include "ext/ui/find_element.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_set>
#include <vector>

// Not `printf`: the release build strips that (src/app/log_strip.h). This page's trace stays on stderr.
#define FLOG(...) std::fprintf(stderr, __VA_ARGS__)

namespace Ext {
namespace FoodEating {

namespace {

// Element ids inside the extension window: its close button is 1; the filter button 102, the cells 1000 +
// index.
constexpr int32_t kFilterButtonId = 102;
constexpr int32_t kRowIdBase = 1000;

constexpr const char* kWindowRsc      = "Dialog/ExtensionsWindow.rsc";
constexpr const char* kCellRsc        = "Dialog/FoodEatingPage.rsc:foodCell";
constexpr const char* kStringTableRsc = "Dialog/ExtensionsStringTable.rsc";

UI::StringTable* OurStringTable() {
    if (!::Object::ExternalFactory::_singleton) return nullptr;
    UI::StringTable* table =
        ::Object::ExternalFactory::_singleton->FindOrLoadAs<UI::StringTable>(
            System::Filename(kStringTableRsc));
    if (!::Object::IsDerivedFromNullSafe(table, UI::StringTable::TypeId())) return nullptr;
    return table;
}

UI::String OurString(const char* key) {
    UI::StringTable* table = OurStringTable();
    if (table == nullptr) return UI::String();
    return UI::String(Object::ExternalPtr<UI::StringTable>(table), System::String(key));
}

UI::TextElement* AsText(UI::Element* e) {
    if (!::Object::IsDerivedFromNullSafe(e, UI::TextElement::TypeId())) return nullptr;
    return static_cast<UI::TextElement*>(e);
}

void SetParameterText(UI::Element* e, const char* text) {
    if (UI::TextElement* t = AsText(e)) t->SetParameter(0, System::String(text));
}

void SetTableText(UI::Element* e, const char* key) {
    UI::TextElement* t = AsText(e);
    if (t == nullptr) return;
    const UI::String s = OurString(key);
    if (s.IsValid()) t->SetText(&s);
}

std::string Lower(const char* s) {
    std::string out(s != nullptr ? s : "");
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// "RabbitMeat" -> "Rabbit Meat": the fallback when a material's own string table does not name it.
std::string PrettyWords(const std::string& in) {
    std::string out;
    for (size_t i = 0; i < in.size(); ++i) {
        const char c = in[i];
        const char p = i ? in[i - 1] : 0;
        const bool up = std::isupper(static_cast<unsigned char>(c)) != 0;
        const bool prevLowOrDigit =
            std::islower(static_cast<unsigned char>(p)) || std::isdigit(static_cast<unsigned char>(p));
        if (i && up && prevLowOrDigit) out.push_back(' ');
        out.push_back(c == '_' ? ' ' : c);
    }
    if (!out.empty()) out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0])));
    return out;
}

// One edible material of the running game.
struct Food {
    const RawMaterialDescription* mat = nullptr;
    std::string name;       // internal name, lower case
    // the name the game shows (the material's own string table), else the internal name in words
    std::string display;
    uint32_t    flags = 0;
    bool        inTown = false;   // the town has had a material of this name
};

// The name of the material's icon in its sprite sheet: the small one the game's lists use, else the full-size
// one;
// null when the material has no sheet or names no sprite.
const char* SpriteName(const RawMaterialDescription* m) {
    if (m == nullptr || m->_spriteSheet == nullptr) return nullptr;
    if (m->_nameSmall._data != nullptr && m->_nameSmall._data[0] != 0) return m->_nameSmall._data;
    if (m->_name._data != nullptr && m->_name._data[0] != 0) return m->_name._data;
    return nullptr;
}

// The raw-material descriptions of the town's known-material list (CitizenBehavior +0x110): a template is
// appended
// by RawMaterialComponent::Startup the first time any entity of it is created and never removed, so this is
// "every
// item the town has ever made or held", the list the Town Hall inventory shows.
std::unordered_set<const void*> TownMaterials() {
    std::unordered_set<const void*> out;
    GameUpdateLoop* game = GameUpdateLoop::_singleton;
    if (game == nullptr || game->m_citizenList == nullptr) return out;
    const auto& list = game->m_citizenList->field_0x110;
    const uint32_t tid = RawMaterialDescription::TypeId();
    for (int32_t i = 0; i < list._size; ++i) {
        const ComponentDescription* cd = list._data[i];
        if (cd == nullptr) continue;
        for (int32_t j = 0; j < cd->_subDescCount; ++j) {
            const Description* sub = cd->_subDescs[j];
            if (sub != nullptr && ::Object::IsDerivedFromNullSafe(sub, tid)) out.insert(sub);
        }
    }
    return out;
}

// Every loaded RawMaterialDescription with the Edible bit, once per internal name, by display name. The
// factory walk is
// the one the game uses to enumerate what a package loaded (ExternalFactory::FindNext).
std::vector<Food> CollectFoods() {
    std::vector<Food> foods;
    ::Object::ExternalFactory* factory = ::Object::ExternalFactory::_singleton;
    if (factory == nullptr) return foods;
    const std::unordered_set<const void*> town = TownMaterials();
    const uint32_t tid = RawMaterialDescription::TypeId();
    const ::Object::External* prev = nullptr;
    int guard = 0;
    while ((prev = factory->FindNext(tid, prev)) != nullptr && guard++ < 20000) {
        if (!::Object::IsDerivedFromNullSafe(prev, tid)) continue;
        const RawMaterialDescription* m = static_cast<const RawMaterialDescription*>(prev);
        if ((m->_flags & kEdible) == 0) continue;
        if (m->_name._data == nullptr || m->_name._data[0] == 0) continue;
        Food f;
        f.mat = m;
        f.name = Lower(m->_name._data);
        f.flags = m->_flags;
        f.inTown = town.count(m) != 0;
        bool dup = false;
        for (Food& g : foods) {
            if (g.name == f.name) {
                g.inTown = g.inTown || f.inTown;
                // the copy that has an icon
                if (SpriteName(g.mat) == nullptr && SpriteName(m) != nullptr) g.mat = m;
                dup = true;
                break;
            }
        }
        if (dup) continue;
        f.display = PrettyWords(m->_name._data);
        if (m->_stringTable != nullptr &&
            ::Object::IsDerivedFromNullSafe(m->_stringTable, UI::StringTable::TypeId())) {
            const int32_t index = m->_stringTable->GetIndexFromName(m->_name);
            if (index >= 0) {
                const System::String text = m->_stringTable->GetText(index);
                if (text._data != nullptr && text._data[0] != 0) f.display = text._data;
            }
        }
        foods.push_back(f);
    }
    std::sort(foods.begin(), foods.end(), [](const Food& a, const Food& b) {
        return Lower(a.display.c_str()) < Lower(b.display.c_str());
    });
    return foods;
}

// -----------------------------------------------------------------------------------------------------------
// The page's state: one page at a time (the extension window is one dialog)
// -----------------------------------------------------------------------------------------------------------
struct Row {
    std::string name;                            // the material's internal name, lower case (the rules' key)
    std::string display;                         // the name the game shows
    uint32_t    flags = 0;                       // its RawMaterialDescription::_flags
    const void* material = nullptr;              // its RawMaterialDescription (the icon)
    bool        inTown = false;                  // the town has had it (CitizenBehavior's material list)
    bool        pending = false;                 // the check box
    Object::InternalPtr<UI::Element> check;      // CheckElement (null while the row is not shown)
    Object::InternalPtr<UI::Element> state;      // (unused: the cell shows no state text)
    Object::InternalPtr<UI::Element> border;     // the cell's border (colour)
};

// every edible food of the game; a cell's id is kRowIdBase + its index
std::vector<Row> g_rows;
bool g_showAll = false;                          // false: only foods the town has had (kept for the session)
Object::InternalPtr<UI::Element> g_table;        // "tableFoodEating"
Object::InternalPtr<UI::Element> g_filterButton; // the filter button (the one child of the button row)

bool Shown(const Row& r) {
    return g_showAll || r.inTown || r.pending;   // a pre-block never vanishes
}

void SetFilterText() {
    UI::Element* button = g_filterButton.Get();
    if (button == nullptr) return;
    const int count = button->ChildCount();
    for (int i = 0; i < count; ++i)
        SetTableText(button->Child(i).Get(), g_showAll ? "FoodEatingFilterAll" : "FoodEatingFilterTown");
}

void RefreshRow(int index) {
    Row& r = g_rows[static_cast<size_t>(index)];
    if (::Object::IsDerivedFromNullSafe(r.check.Get(), UI::CheckElement::TypeId()))
        static_cast<UI::CheckElement*>(r.check.Get())->_checked = r.pending ? 1 : 0;
    // Red-tinted when blocked, the Mods list's white otherwise.
    if (r.border.Get() != nullptr) r.border->SetColor(r.pending ? 0x20ffa0a0u : 0x10ffffffu);
}

// (Re)creates the cells of the shown foods in the four-column table, as TownHallUI::InitInventory fills its
// inventory
// table: size the table (rows = foods / 4, rounded up), then SetChild a clone of the cell template into each
// slot. The
// foods and their check state live in g_rows, so a rebuild keeps them.
void BuildRows() {
    if (g_table.Get() == nullptr) return;
    if (!::Object::IsDerivedFromNullSafe(g_table.Get(), UI::TableElement::TypeId())) {
        FLOG("[FoodEating] tableFoodEating is not a TableElement\n");
        return;
    }
    UI::TableElement* table = static_cast<UI::TableElement*>(g_table.Get());
    SetFilterText();

    const UI::Dialog* dialog = nullptr;
    const ::Object::External* dlgExt =
        ::Object::ExternalFactory::_singleton->FindOrLoad(System::Filename(kWindowRsc));
    if (::Object::IsDerivedFromNullSafe(dlgExt, UI::Dialog::TypeId()))
        dialog = static_cast<const UI::Dialog*>(dlgExt);
    const ::Object::External* cellExt =
        ::Object::ExternalFactory::_singleton->FindOrLoad(System::Filename(kCellRsc));
    if (dialog == nullptr || !::Object::IsDerivedFromNullSafe(cellExt, UI::ElementDescription::TypeId())) {
        FLOG("[FoodEating] %s / %s did not load: no cells\n", kWindowRsc, kCellRsc);
        return;
    }
    UI::ElementDescription* cellTemplate =
        const_cast<UI::ElementDescription*>(static_cast<const UI::ElementDescription*>(cellExt));

    std::vector<size_t> shownRows;
    for (size_t i = 0; i < g_rows.size(); ++i) {
        Row& r = g_rows[i];
        r.check = Object::InternalPtr<UI::Element>();
        r.border = Object::InternalPtr<UI::Element>();
        if (Shown(r)) shownRows.push_back(i);
    }
    constexpr int kColumns = 4;   // FoodEatingPage.rsc:tableFoodEating's _width
    const int rowCount = (static_cast<int>(shownRows.size()) + kColumns - 1) / kColumns;
    table->SetSize(rowCount, kColumns);                      // clears the old cells

    int made = 0;
    for (size_t k = 0; k < shownRows.size(); ++k) {
        const size_t i = shownRows[k];
        Row& r = g_rows[i];
        Object::InternalPtr<UI::Element> cell;
        cellTemplate->CreateInstance(&cell, dialog);
        if (cell.Get() == nullptr) break;
        table->SetChild(&cell, static_cast<int32_t>(k));

        auto child = [&cell](const char* name) {
            return cell->FindElement(System::String(name)).Get();
        };

        // The label: the food's own icon (the small one the game's lists use) and its name.
        UI::Element* labelElem = child("labelFoodEating");
        SetTableText(labelElem, "FoodEatingParam");
        SetParameterText(labelElem, r.display.c_str());
        const RawMaterialDescription* mat = static_cast<const RawMaterialDescription*>(r.material);
        if (const char* spriteName = SpriteName(mat)) {
            if (::Object::IsDerivedFromNullSafe(labelElem, UI::LabelElement::TypeId())) {
                UI::Sprite sprite(Object::ExternalPtr<UI::SpriteSheet>(mat->_spriteSheet),
                                  System::String(spriteName));
                if (sprite.IsValid()) static_cast<UI::LabelElement*>(labelElem)->SetSprite(&sprite);
            }
        }

        r.check = Object::InternalPtr<UI::Element>(child("checkFoodEating"));
        Object::InternalPtr<UI::Element> border;
        r.border = *cell->GetBorder(&border);
        if (r.check.Get() != nullptr) r.check->SetElementId(kRowIdBase + static_cast<int32_t>(i));
        RefreshRow(static_cast<int>(i));
        ++made;
    }
    FLOG("[FoodEating] %d of %d food(s) shown in %d row(s)\n", made, static_cast<int>(g_rows.size()),
         rowCount);
}

// One check box was used: fold that one food into the town's rules, live. The box then shows what the rules
// say (an
// allow: entry may keep a food eaten, for instance).
void ApplyRow(int index) {
    Row& r = g_rows[static_cast<size_t>(index)];
    RuleSet rules = CurrentRules();
    bool changed = false;
    if (r.pending != IsBlockedNow(r.flags, r.name)) changed = SetBlocked(rules, r.flags, r.name, r.pending);
    if (changed) ApplyRules(rules);
    r.pending = IsBlockedNow(r.flags, r.name);
    RefreshRow(index);
    FLOG("[FoodEating] %s -> %s, rules changed=%d -> \"%s\"\n", r.name.c_str(),
         r.pending ? "not eaten" : "eaten", changed ? 1 : 0, FormatRules(CurrentRules()).c_str());
}

void PageOpen(UI::Element* page) {
    // The filter button is the one child of its row, not found by name: it derives from buttonOk and a
    // derived element
    // can carry its base's name.
    UI::Element* filter = nullptr;
    if (UI::Element* row = page->FindElement(System::String("buttonFoodEatingRow")).Get()) {
        if (row->_children.GetSize() > 0) filter = row->_children.GetData()[0];
    }
    if (filter != nullptr) filter->SetElementId(kFilterButtonId);
    g_filterButton = Object::InternalPtr<UI::Element>(filter);

    g_table = Object::InternalPtr<UI::Element>(page->FindElement(System::String("tableFoodEating")).Get());
    if (g_table.Get() == nullptr) {
        FLOG("[FoodEating] no tableFoodEating in the page: empty page\n");
        return;
    }

    // Every edible food of the game, with whether the town has had it (the Town Hall inventory's list). The
    // page
    // opens on the foods the town has had; a town that has had none (or the "all" choice of the last opening)
    // shows all.
    const std::vector<Food> foods = CollectFoods();
    g_rows.clear();
    g_rows.reserve(foods.size());
    bool anyInTown = false;
    for (const Food& food : foods) {
        Row r;
        r.name = food.name;
        r.display = food.display;
        r.flags = food.flags;
        r.material = food.mat;
        r.inTown = food.inTown;
        r.pending = IsBlockedNow(food.flags, food.name);
        anyInTown = anyInTown || food.inTown;
        g_rows.push_back(r);
    }
    if (!anyInTown) g_showAll = true;
    BuildRows();
    FLOG("[FoodEating] page opened: %d food(s), %s\n", static_cast<int>(g_rows.size()),
         g_showAll ? "all foods" : "the town's foods");
}

bool PageChanged(UI::Element* /*page*/, int32_t id) {
    if (id == kFilterButtonId) {
        // The filter: foods the town has had <-> all foods. The checks are kept; only the rows are rebuilt.
        g_showAll = !g_showAll;
        BuildRows();
        return true;
    }
    if (id >= kRowIdBase && id < kRowIdBase + static_cast<int32_t>(g_rows.size())) {
        const int index = id - kRowIdBase;
        Row& r = g_rows[static_cast<size_t>(index)];
        if (::Object::IsDerivedFromNullSafe(r.check.Get(), UI::CheckElement::TypeId()))
            r.pending = static_cast<UI::CheckElement*>(r.check.Get())->_checked != 0;
        else
            r.pending = !r.pending;
        ApplyRow(index);
        return true;
    }
    return false;
}

void PageClose() {
    g_rows.clear();
    g_table = Object::InternalPtr<UI::Element>();
    g_filterButton = Object::InternalPtr<UI::Element>();
}

}  // namespace
}  // namespace FoodEating

namespace Toolbar {

const Contribution& FoodEatingPage() {
    static const Contribution c = { "food_eating", "FoodEatingTab",
                                    "Dialog/FoodEatingPage.rsc:pageFoodEating",
                                    FoodEating::PageOpen, FoodEating::PageChanged, FoodEating::PageClose };
    return c;
}

}  // namespace Toolbar
}  // namespace Ext
