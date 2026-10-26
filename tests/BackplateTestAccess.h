#pragma once

#include "FD2D/Backplate.h"

// FD2D's test access to Backplate internals, shared by the FICture2 tests
// (one definition per test executable).

namespace FD2D
{
    struct BackplateTestAccess
    {
        static void TargetRecreated(Backplate& backplate)
        {
            backplate.InvalidateGraphics(GraphicsInvalidationReason::TargetRecreated, false, true, false);
        }

        static void DeviceLost(Backplate& backplate)
        {
            backplate.DiscardDeviceResources();
            backplate.InvalidateGraphics(GraphicsInvalidationReason::DeviceLost, true, true, false);
        }
    };
}
