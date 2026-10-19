// tests/unit/main.cpp -- runs every UNIT_TEST in link order. Exit code = failures (a
// skipped test, UNIT_SKIP in unit.h, is counted and listed but never fails the run).
#include "unit.h"

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    int tests = 0;
    for (unit::Test* t = unit::Head(); t; t = t->next) {
        const int before = unit::Failures();
        const int skippedBefore = unit::Skipped();
        unit::Current() = t->name;
        t->fn();
        unit::Current() = nullptr;
        ++tests;
        const char* verdict = unit::Failures() != before ? "FAIL"
                            : unit::Skipped() != skippedBefore ? "skip" : "ok  ";
        std::printf("%s %s\n", verdict, t->name);
    }
    std::printf("%d tests, %d failures, %d skipped\n", tests, unit::Failures(), unit::Skipped());
    for (unit::Skip* s = unit::Skips(); s; s = s->next)
        std::printf("  skipped %s: %s\n", s->name, s->reason);
    return unit::Failures() ? 1 : 0;
}
