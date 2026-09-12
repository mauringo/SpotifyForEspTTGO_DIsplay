#include "ButtonClicks.h"
#include <assert.h>

int main()
{
    ButtonClicks clicks;
    assert(clicks.press(100) == ButtonAction::None);
    assert(clicks.tick(449) == ButtonAction::None);
    assert(clicks.tick(450) == ButtonAction::TogglePlayback);
    assert(clicks.tick(1000) == ButtonAction::None); // Holding does not repeat.
    assert(clicks.press(1100) == ButtonAction::None);
    assert(clicks.press(1449) == ButtonAction::SaveTrack);
    assert(clicks.tick(1800) == ButtonAction::None); // No toggle after saving.
    assert(clicks.press(2000) == ButtonAction::None);
    assert(clicks.tick(2350) == ButtonAction::TogglePlayback);
    assert(clicks.press(2350) == ButtonAction::None);
    assert(clicks.tick(2700) == ButtonAction::TogglePlayback);
    const uint32_t nearWrap = UINT32_MAX - 100;
    assert(clicks.press(nearWrap) == ButtonAction::None);
    assert(clicks.press(99) == ButtonAction::SaveTrack);
    assert(clicks.press(nearWrap) == ButtonAction::None);
    assert(clicks.tick(249) == ButtonAction::TogglePlayback);
}
