#pragma once

#include "Config/Config.h"
#include "Controller/AnimatedLoadingScreenController.h"

namespace ALS::D3D11Hooks
{
    bool Install(AnimatedLoadingScreenController& controller);
    void Uninstall();
    bool IsInstalled();
}

