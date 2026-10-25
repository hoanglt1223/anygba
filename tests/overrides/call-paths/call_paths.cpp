// Counts how each overridden function was reached and prints the counts at exit; the original
// translation does the work (tests/override-e2e.cmake).
#include <anygba/override.hpp>

#include <cstdio>
#include <cstdlib>

namespace {

struct Count {
    const char* name;
    unsigned long long recompiled = 0;
    unsigned long long other = 0;
};

Count g_counts[] = {{"direct"}, {"indirect"}, {"interpreted"}};

void report() {
    for (const Count& c : g_counts) {
        std::printf("call-paths %s: recompiled=%llu other=%llu\n", c.name, c.recompiled, c.other);
    }
}

[[maybe_unused]] const bool g_report_registered = std::atexit(report) == 0;

void count_and_run(Count& count, anygba::guest::Guest& g) {
    ++(g.from_recompiled_code() ? count.recompiled : count.other);
    g.run_original();
}

}  // namespace

ANYGBA_OVERRIDE(call_paths_direct, g) { count_and_run(g_counts[0], g); }
ANYGBA_OVERRIDE(call_paths_indirect, g) { count_and_run(g_counts[1], g); }
ANYGBA_OVERRIDE(call_paths_interpreted, g) { count_and_run(g_counts[2], g); }
