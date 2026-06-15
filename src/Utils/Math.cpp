#include "Utils/Math.h"

#include <algorithm>

namespace ALS
{
    float Clamp01(double value)
    {
        return static_cast<float>(std::clamp(value, 0.0, 1.0));
    }

    float FadeInAlpha(double elapsedMs, double durationMs)
    {
        if (durationMs <= 0.0) {
            return 1.0F;
        }
        return Clamp01(elapsedMs / durationMs);
    }

    float FadeOutAlpha(double elapsedMs, double durationMs)
    {
        if (durationMs <= 0.0) {
            return 0.0F;
        }
        return Clamp01(1.0 - (elapsedMs / durationMs));
    }
}

