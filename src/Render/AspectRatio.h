#pragma once

#include "Config/Config.h"

namespace ALS
{
    struct RectF
    {
        float x{ 0.0F };
        float y{ 0.0F };
        float width{ 0.0F };
        float height{ 0.0F };
    };

    struct AspectMapping
    {
        RectF destination{};
        RectF uv{ 0.0F, 0.0F, 1.0F, 1.0F };
    };

    [[nodiscard]] AspectMapping CalculateAspectMapping(
        FitMode fitMode,
        float viewportWidth,
        float viewportHeight,
        float mediaWidth,
        float mediaHeight);
}

