#pragma once
// ============================================================================
// src/ext/high_limits/high_limits.h -- ext.high_limits (class B, default OFF)
// ============================================================================
// NOT RETAIL. docs/extensions/handins/HIGHLIMITS.md.
//
// The town's per-resource limit (CitizenBehavior::professionResources[i].maxCount) is an int32 that the game
// stores, saves and compares without any upper bound. The spinner that sets it stops at 999,999 for two reasons,
// both in the UI:
//   * ResourceLimitUI::OnCreated gives its "editLimit" spinner SetMinMax(0, 999999), and
//   * the spinner's edit field takes only 6 typed characters (the description spinnerEditSmall6 has
//     _maxLength 6 and _maxValue 999999).
// So the arrows could push a value past 999,999 one click at a time, but a typed value could not.
// With the row on, the spinner takes 0..999,999,999 and 9 typed digits, and every limit box gets a minimum
// width that fits nine digits so a window's columns stay aligned.
//
// ONE controller serves every limit spinner (the Town Hall's Production tab, the Limits window, and the dialog
// of each production building that shows its limit), so ONE seam in ResourceLimitUI::OnCreated covers them
// all. Retail's body is kept in the else arm.
//
// Class B (DESIGN.md section 2): no serialized state, but a limit above 999,999 is a value the faithful
// spinner cannot show, so the row is locked for the game and recorded in the save.
//
// Standard C++ only here: tests/unit/ext_high_limits_test.cpp links high_limits.cpp on its own. The spinner
// glue is spinner.cpp (exe only).
// ============================================================================
#include <cstdint>

namespace UI {
class SpinnerElement;
}

namespace Ext {
namespace HighLimits {

constexpr int32_t kRetailMax = 999999;   // ResourceLimitUI::OnCreated's SetMinMax and the description's _maxValue
constexpr int32_t kHighMax = 999999999;  // nine digits; fits an int32 (2,147,483,647)
constexpr int32_t kRetailDigits = 6;     // the description's _maxLength
constexpr int32_t kHighDigits = 9;
constexpr int32_t kRetailMinWidth = 42;  // the description's _minWidth in pixels: room for 6 digits
constexpr int32_t kHighMinWidth = 64;    // room for 9 digits at FontSmall; every limit box gets the same width

// Whether the row is on for this game (the game's locked value).
bool Active();

// What the limit spinners take now: the retail values while the row is off.
int32_t MaxValue();
int32_t MaxDigits();
int32_t MinWidth();

// ResourceLimitUI::OnCreated's seam: the range, the typed-digit limit and the minimum width of the spinner's
// edit field. Exe only (spinner.cpp).
void ApplyToSpinner(UI::SpinnerElement* spinner);

}  // namespace HighLimits
}  // namespace Ext
