#pragma once

namespace movement
{
    // Follows a planned looping patrol route relative to the position where
    // autowalk was first enabled. Releases all movement keys when disabled.
    void update(bool enabled);
    void stop();
}
