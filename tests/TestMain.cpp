#include "Logging/Log.h"
#include "TestFramework.h"

#include <iostream>

int main()
{
    ALS::Log::InitializeConsoleLoggerForTests("warn");

    int failed = 0;
    for (const auto& test : ALS::Tests::Registry()) {
        try {
            test.run();
            std::cout << "[pass] " << test.name << '\n';
        } catch (const std::exception& e) {
            ++failed;
            std::cerr << "[fail] " << test.name << ": " << e.what() << '\n';
        }
    }

    if (failed != 0) {
        std::cerr << failed << " test(s) failed.\n";
        return 1;
    }

    std::cout << ALS::Tests::Registry().size() << " test(s) passed.\n";
    return 0;
}

