#include "../capturepace.h"

#include <cstdlib>
#include <iostream>
#include <limits>

namespace {
unsigned passed = 0;

void expect(const char* name, bool condition)
{
    if (!condition) {
        std::cerr << "FAIL " << name << '\n';
        std::exit(1);
    }
    ++passed;
}
} // namespace

int main()
{
    twitchstat::CapturePace pace;
    expect("new scheduler starts ready at zero", pace.ready(0));
    expect("new scheduler starts ready late in clock range", pace.ready(0xfffffff0u));

    pace.defer(100, 50);
    expect("queued duplicate at capture finish is denied", !pace.ready(100));
    expect("another queued duplicate is still denied", !pace.ready(100));
    expect("tick just before deadline is denied", !pace.ready(149));
    expect("deadline allows the next capture", pace.ready(150));
    expect("late timer still permits one capture", pace.ready(180));

    // Capture started at 150 and finished at 400. A duplicate must not use the expired
    // deadline from its start, even when the work took longer than the desired pause.
    pace.defer(400, 50);
    expect("long capture gets cooldown from finish", !pace.ready(400));
    expect("long capture receives the entire cooldown", !pace.ready(449));
    expect("long capture cooldown eventually ends", pace.ready(450));

    const auto maxTick = (std::numeric_limits<std::uint32_t>::max)();
    pace.defer(maxTick - 20, 50); // Deadline wraps to 29.
    expect("wrapping deadline blocks the finish tick", !pace.ready(maxTick - 20));
    expect("wrapping deadline blocks before clock wrap", !pace.ready(maxTick));
    expect("wrapping deadline blocks immediately after wrap", !pace.ready(0));
    expect("wrapping deadline blocks the last early tick", !pace.ready(28));
    expect("wrapping deadline permits exact wrapped deadline", pace.ready(29));
    expect("wrapping deadline permits a late tick", pace.ready(30));

    pace.defer(1000, 50);
    pace.reset();
    expect("reset cancels a pending cooldown immediately", pace.ready(1000));
    expect("reset is independent of an old clock value", pace.ready(0));
    pace.defer(0, 50);
    expect("scheduler can defer again after reset", !pace.ready(49));
    expect("scheduler can become ready again after reset", pace.ready(50));
    pace.defer(50, 0);
    expect("zero cooldown permits the same tick", pace.ready(50));

    std::cout << passed << " capture pacing checks passed\n";
    return 0;
}
