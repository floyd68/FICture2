#pragma once

#include <d2d1.h>

#include <cmath>

namespace FD2D
{
    struct SelectionStyle
    {
        D2D1_COLOR_F accent { 1.0f, 0.60f, 0.24f, 1.0f };
        D2D1_COLOR_F shadow { 0.0f, 0.0f, 0.0f, 0.55f };
        D2D1_COLOR_F fill { 1.0f, 1.0f, 1.0f, 1.0f };

        float radius { 6.0f };
        float baseInflate { 1.0f };
        float popInflate { 4.0f };
        float shadowThickness { 3.0f };
        float accentThickness { 2.0f };
        float fillMaxAlpha { 0.10f };

        bool breatheEnabled { true };
        int breathePeriodMs { 900 };
        // Whole breathe cycles after a selection; then the outline rests and no
        // more frames are requested (no permanent render heartbeat).
        int breatheCycles { 3 };
        float breatheInflateAmp { 1.40f };
        float breatheThicknessAmp { 1.10f };
        float breatheAlphaAmp { 0.25f };
    };

    // Bounded selection "breathe" amount in [0, 1] at `elapsedMs` after the
    // selection: starts at rest, follows whole cosine cycles so it ends exactly at
    // rest, and stops after `cycles` cycles. `active` is true while another frame
    // is needed.
    inline float SelectionBreathe(unsigned long long elapsedMs, int periodMs, int cycles, bool& active)
    {
        active = false;
        if (periodMs <= 0 || cycles <= 0)
        {
            return 0.0f;
        }
        const unsigned long long period = static_cast<unsigned long long>(periodMs);
        if (elapsedMs >= period * static_cast<unsigned long long>(cycles))
        {
            return 0.0f;
        }
        active = true;
        const float phase = static_cast<float>(elapsedMs % period) / static_cast<float>(period) * 6.28318530718f;
        return 0.5f - 0.5f * std::cos(phase);
    }
}
