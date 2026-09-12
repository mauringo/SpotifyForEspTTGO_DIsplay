#pragma once
#include <stdint.h>

enum class ButtonAction : uint8_t { None, TogglePlayback, NextTrack, SaveTrack };

// Feed debounced press edges from the sampling task, even while HTTP blocks.
class ButtonClicks
{
public:
    ButtonAction press(uint32_t now)
    {
        if (pending_ && uint32_t(now - firstPress_) < 350)
        {
            pending_ = false;
            return ButtonAction::SaveTrack;
        }
        pending_ = true;
        firstPress_ = now;
        return ButtonAction::None;
    }

    ButtonAction tick(uint32_t now)
    {
        if (pending_ && uint32_t(now - firstPress_) >= 350)
        {
            pending_ = false;
            return ButtonAction::TogglePlayback;
        }
        return ButtonAction::None;
    }

private:
    bool pending_ = false;
    uint32_t firstPress_ = 0;
};
