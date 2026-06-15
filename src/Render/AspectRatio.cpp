#include "Render/AspectRatio.h"

#include <algorithm>

namespace ALS
{
    AspectMapping CalculateAspectMapping(
        FitMode fitMode,
        float viewportWidth,
        float viewportHeight,
        float mediaWidth,
        float mediaHeight)
    {
        AspectMapping mapping;
        mapping.destination = RectF{ 0.0F, 0.0F, viewportWidth, viewportHeight };
        mapping.uv = RectF{ 0.0F, 0.0F, 1.0F, 1.0F };

        if (viewportWidth <= 0.0F || viewportHeight <= 0.0F || mediaWidth <= 0.0F || mediaHeight <= 0.0F) {
            return mapping;
        }

        if (fitMode == FitMode::Stretch) {
            return mapping;
        }

        const auto viewportAspect = viewportWidth / viewportHeight;
        const auto mediaAspect = mediaWidth / mediaHeight;

        if (fitMode == FitMode::Contain) {
            if (mediaAspect > viewportAspect) {
                const auto height = viewportWidth / mediaAspect;
                mapping.destination = RectF{ 0.0F, (viewportHeight - height) * 0.5F, viewportWidth, height };
            } else {
                const auto width = viewportHeight * mediaAspect;
                mapping.destination = RectF{ (viewportWidth - width) * 0.5F, 0.0F, width, viewportHeight };
            }
            return mapping;
        }

        if (fitMode == FitMode::Cover) {
            if (mediaAspect > viewportAspect) {
                const auto visibleWidth = viewportAspect / mediaAspect;
                mapping.uv = RectF{ (1.0F - visibleWidth) * 0.5F, 0.0F, visibleWidth, 1.0F };
            } else {
                const auto visibleHeight = mediaAspect / viewportAspect;
                mapping.uv = RectF{ 0.0F, (1.0F - visibleHeight) * 0.5F, 1.0F, visibleHeight };
            }
        }

        return mapping;
    }
}

