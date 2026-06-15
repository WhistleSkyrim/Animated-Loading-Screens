#include "Render/AspectRatio.h"
#include "TestFramework.h"

#include <cmath>

namespace
{
    void Near(float actual, float expected, const char* message)
    {
        ALS::Tests::Expect(std::fabs(actual - expected) < 0.001F, message);
    }
}

ALS_TEST(AspectRatioCalculatesCoverContainAndStretch)
{
    const auto contain = ALS::CalculateAspectMapping(ALS::FitMode::Contain, 1920.0F, 1080.0F, 1000.0F, 1000.0F);
    Near(contain.destination.x, 420.0F, "contain x");
    Near(contain.destination.y, 0.0F, "contain y");
    Near(contain.destination.width, 1080.0F, "contain width");
    Near(contain.destination.height, 1080.0F, "contain height");

    const auto cover = ALS::CalculateAspectMapping(ALS::FitMode::Cover, 1920.0F, 1080.0F, 1000.0F, 1000.0F);
    Near(cover.destination.width, 1920.0F, "cover width");
    Near(cover.uv.y, 0.21875F, "cover vertical crop");
    Near(cover.uv.height, 0.5625F, "cover visible height");

    const auto stretch = ALS::CalculateAspectMapping(ALS::FitMode::Stretch, 1920.0F, 1080.0F, 1000.0F, 1000.0F);
    Near(stretch.destination.width, 1920.0F, "stretch width");
    Near(stretch.uv.width, 1.0F, "stretch uv width");
}

