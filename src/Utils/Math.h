#pragma once

namespace ALS
{
    [[nodiscard]] float Clamp01(double value);
    [[nodiscard]] float FadeInAlpha(double elapsedMs, double durationMs);
    [[nodiscard]] float FadeOutAlpha(double elapsedMs, double durationMs);
}

