#include "checkbox_tap.h"
#include <assert.h>
#include <stdio.h>

static unsigned release(checkbox_tap_t *tap) {
    assert(checkbox_tap_update(tap, false, false, 0) == 0);
    assert(checkbox_tap_update(tap, false, false, 0) == 0);
    return checkbox_tap_update(tap, false, false, 0);
}

int main(void) {
    checkbox_tap_t tap = {0};
    // Holding and transient failed ADC samples never toggle repeatedly.
    for (unsigned i = 0; i < 100; ++i)
        assert(checkbox_tap_update(&tap, true, i % 3 != 1, 7) == 0);
    assert(release(&tap) == 7);
    assert(release(&tap) == 0);
    // Re-tapping the same item is a new action (complete/reopen).
    checkbox_tap_update(&tap, true, true, 7);
    assert(release(&tap) == 7);
    // A brief PENIRQ bounce does not terminate the press.
    checkbox_tap_update(&tap, true, true, 8);
    checkbox_tap_update(&tap, false, false, 0);
    checkbox_tap_update(&tap, true, true, 8);
    assert(release(&tap) == 8);
    // Dragging onto another checkbox or starting outside cancels.
    checkbox_tap_update(&tap, true, true, 7);
    checkbox_tap_update(&tap, true, true, 8);
    assert(release(&tap) == 0);
    checkbox_tap_update(&tap, true, true, 0);
    checkbox_tap_update(&tap, true, true, 7);
    assert(release(&tap) == 0);
    // Incoming BLE items must not turn a held tap into the wrong action.
    checkbox_tap_update(&tap, true, true, 9);
    tap.target = 0;
    assert(release(&tap) == 0);
    puts("Checkbox interaction tests passed");
}
