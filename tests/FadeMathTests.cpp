#include "Utils/Math.h"
#include "TestFramework.h"

ALS_TEST(FadeMathClampsAndHandlesZeroDurations)
{
    ALS::Tests::ExpectEq(ALS::FadeInAlpha(0.0, 100.0), 0.0F, "fade in starts at zero");
    ALS::Tests::ExpectEq(ALS::FadeInAlpha(50.0, 100.0), 0.5F, "fade in midpoint");
    ALS::Tests::ExpectEq(ALS::FadeInAlpha(150.0, 100.0), 1.0F, "fade in clamps");
    ALS::Tests::ExpectEq(ALS::FadeInAlpha(0.0, 0.0), 1.0F, "zero fade in is immediate");

    ALS::Tests::ExpectEq(ALS::FadeOutAlpha(0.0, 100.0), 1.0F, "fade out starts at one");
    ALS::Tests::ExpectEq(ALS::FadeOutAlpha(50.0, 100.0), 0.5F, "fade out midpoint");
    ALS::Tests::ExpectEq(ALS::FadeOutAlpha(150.0, 100.0), 0.0F, "fade out clamps");
}

