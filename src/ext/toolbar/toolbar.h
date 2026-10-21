// ============================================================================
// src/ext/toolbar/toolbar.h -- the in-game extension window: ONE toolbar button, one page per active
// extension
// ============================================================================
// NOT RETAIL. docs/extensions/DESIGN.md "Surfaces" (ruled 2026-10-20), docs/extensions/handins/RESTRICTEDFOODS.md.
// An extension
// with an in-game control does not get a button of its own: the main toolbar gets ONE extension button, in
// its Reports
// group (Status Bar, Limits, Town Hall, Help), and it opens one window with one tab per extension that
// contributes a page.
// With no active contributor the button is not added and the toolbar is retail's.
//
// A contribution keeps its own element pointers (found by name in its page); this host holds only the list.
// The page's
// controls are live: `changed` writes the extension's state at once (no Apply). The lock (class B / C) is on
// the
// extension's ACTIVATION, so a page is offered only while the row is active for this game; what the page
// edits may change
// whenever the player likes (DESIGN.md "What the lock covers").
//
// The host's arms (the one EXT SEAM line per site):
//   src/game/Dialog/toolbardialog.cpp   InsertAt / Style while the Reports group is built, Toggle on the
//   button's click
//
// Standard C++ for the list and Offered (tests/unit/ext_toolbar_test.cpp links toolbar.cpp alone); the window
// and the
// button are toolbar_window.cpp (exe only).
// ============================================================================
#pragma once

#include <cstdint>

namespace UI {
class Element;
class UserInterface;
}
class Toolbar;   // Game/Toolbar.h: the retail toolbar item (global)

namespace Ext {
namespace Toolbar {

struct Contribution {
    const char* id;        // the Entry id; the page is offered only while that row is active for the game
    const char* caption;   // the tab's text: a key in Dialog/ExtensionsStringTable.rsc
    // "Dialog/RestrictedFoodsPage.rsc:pageRestrictedFoods": our text .rsc, the page's root object
    const char* page;
    void (*open)(UI::Element* page);                   // find the elements by name, fill them from the state
    // a control (element id `id`) was used: write the state now; false = not mine
    bool (*changed)(UI::Element* page, int32_t id);
    void (*close)();                                   // forget the element pointers
};

// The contributions (defined beside their extensions, src/ext/<id>/toolbar_page.cpp).
const Contribution& RestrictedFoodsPage();

// The list, and the ones whose row is active for this game (null-terminated into `out`; returns the count).
const Contribution* const* All(int* n);
int ActivePages(const Contribution** out, int capacity);

// Whether the host adds its button: at least one active contributor, and not under the gate's command-line
// faithful profile.
bool Offered();

// ---- the button (toolbar_window.cpp; EXT SEAMS in ToolbarDialog) ----
// Where in the sorted `items` (count `n`) of the Reports group the button goes, or -1 for none: not the
// Reports group,
// no running game, not Offered(), or the resources do not load. The toolbar keeps a null entry in its item
// array for it,
// so each retail reader of that array skips null.
int InsertAt(const ::Toolbar* group, const ::Toolbar* const* items, int n);
// Name, id, tooltip and icon on the freshly cloned toolbar button `button` (a template-3 clone) at `index`.
void Style(UI::Element* button, int32_t index);
// The button's click: opens the window, or closes it when it is open.
void Toggle(UI::UserInterface* ui);

}  // namespace Toolbar
}  // namespace Ext
