cmake_minimum_required(VERSION 3.26)

if(POLICY CMP0207)
    cmake_policy(SET CMP0207 NEW)
endif()

project(AnimatedLoadingScreens VERSION 0.1.0 LANGUAGES CXX)

option(ALS_BUILD_PLUGIN "Build the SKSE plugin DLL" ON)
option(ALS_ENABLE_FFMPEG "Enable FFmpeg video decoding support" ON)
option(ALS_BUILD_TESTS "Build unit tests for non-Skyrim logic" ON)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Animated Loading Screens must be built as x64.")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

find_package(spdlog CONFIG QUIET)
set(ALS_HAS_SPDLOG 0)
if(spdlog_FOUND)
    set(ALS_HAS_SPDLOG 1)
else()
    message(WARNING "spdlog was not found. Building with the internal fallback logger. Install spdlog for production plugin builds.")
    add_library(spdlog::spdlog INTERFACE IMPORTED)
endif()

set(ALS_CORE_SOURCES
    src/Config/Config.cpp
    src/Controller/AnimatedLoadingScreenController.cpp
    src/Logging/Log.cpp
    src/Media/MediaLibrary.cpp
    src/Media/MediaSelector.cpp
    src/Media/Playlist.cpp
    src/Render/AspectRatio.cpp
    src/Utils/Math.cpp
    src/Utils/Paths.cpp
)

add_library(AnimatedLoadingScreensCore STATIC ${ALS_CORE_SOURCES})
add_library(ALS::Core ALIAS AnimatedLoadingScreensCore)

target_include_directories(AnimatedLoadingScreensCore
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_compile_definitions(AnimatedLoadingScreensCore
    PUBLIC
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        ALS_VERSION_STRING="${PROJECT_VERSION}"
        ALS_HAS_SPDLOG=${ALS_HAS_SPDLOG}
)

target_link_libraries(AnimatedLoadingScreensCore
    PUBLIC
        spdlog::spdlog
)

if(MSVC)
    target_compile_options(AnimatedLoadingScreensCore PRIVATE /W4 /permissive- /Zc:preprocessor)
else()
    target_compile_options(AnimatedLoadingScreensCore PRIVATE -Wall -Wextra -Wpedantic)
    endif()

if(ALS_BUILD_PLUGIN)
    find_package(directxtk CONFIG REQUIRED)
    if(NOT TARGET Microsoft::DirectXTK)
        message(FATAL_ERROR "directxtk was found, but the Microsoft::DirectXTK CMake target was not exported.")
    endif()

    find_package(CommonLibSSE CONFIG REQUIRED)
    find_package(minhook CONFIG REQUIRED)

    if(ALS_ENABLE_FFMPEG)
        find_package(FFmpeg REQUIRED COMPONENTS avformat avcodec avutil swscale)
    endif()

    set(ALS_PLUGIN_SOURCES
        src/Decode/FFmpegVideoDecoder.cpp
        src/Plugin.cpp
        src/Render/D3D11Hooks.cpp
        src/Render/D3D11OverlayRenderer.cpp
        src/Render/SkyrimSwapChain.cpp
        src/Runtime/LoadingMenuWatcher.cpp
    )

    add_commonlibsse_plugin(AnimatedLoadingScreens
        AUTHOR "Animated Loading Screens contributors"
        MINIMUM_SKSE_VERSION 2.0.20.0
        SOURCES
            ${ALS_PLUGIN_SOURCES}
    )

    if(MSVC)
        target_compile_options(AnimatedLoadingScreens PRIVATE "/FI\"${CMAKE_CURRENT_SOURCE_DIR}/src/CommonLibCompat.h\"")
    endif()

    target_include_directories(AnimatedLoadingScreens
        PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )

    target_compile_definitions(AnimatedLoadingScreens
        PRIVATE
            WIN32_LEAN_AND_MEAN
            NOMINMAX
            ALS_ENABLE_FFMPEG=$<BOOL:${ALS_ENABLE_FFMPEG}>
    )

    set(ALS_MINHOOK_TARGET "")
    if(TARGET minhook::minhook)
        set(ALS_MINHOOK_TARGET minhook::minhook)
    elseif(TARGET MinHook::MinHook)
        set(ALS_MINHOOK_TARGET MinHook::MinHook)
    elseif(TARGET minhook)
        set(ALS_MINHOOK_TARGET minhook)
    else()
        message(FATAL_ERROR "minhook was found, but no known CMake target was exported.")
    endif()

    target_link_libraries(AnimatedLoadingScreens
        PRIVATE
            ALS::Core
            CommonLibSSE::CommonLibSSE
            ${ALS_MINHOOK_TARGET}
            d3d11
            dxgi
    )

    if(ALS_ENABLE_FFMPEG)
        target_include_directories(AnimatedLoadingScreens
            PRIVATE
                "${FFmpeg_avformat_INCLUDE_DIR}"
                "${FFmpeg_avcodec_INCLUDE_DIR}"
                "${FFmpeg_avutil_INCLUDE_DIR}"
                "${FFmpeg_swscale_INCLUDE_DIR}"
        )
    endif()

    set_target_properties(AnimatedLoadingScreens PROPERTIES
        OUTPUT_NAME "AnimatedLoadingScreens"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )

    add_custom_command(TARGET AnimatedLoadingScreens POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/resources/AnimatedLoadingScreens.ini"
            "$<TARGET_FILE_DIR:AnimatedLoadingScreens>/AnimatedLoadingScreens.ini"
        COMMENT "Copying default AnimatedLoadingScreens.ini next to the plugin"
    )

    add_custom_command(TARGET AnimatedLoadingScreens POST_BUILD
        COMMAND "${CMAKE_COMMAND}"
            "-DALS_RUNTIME_DLLS=$<JOIN:$<TARGET_RUNTIME_DLLS:AnimatedLoadingScreens>,;>"
            "-DALS_RUNTIME_DLL_DESTINATION=$<TARGET_FILE_DIR:AnimatedLoadingScreens>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CopyRuntimeDlls.cmake"
        COMMENT "Copying runtime DLL dependencies next to the plugin"
        VERBATIM
    )

    install(TARGETS AnimatedLoadingScreens
        RUNTIME_DEPENDENCY_SET AnimatedLoadingScreensRuntimeDeps
        RUNTIME DESTINATION "Data/SKSE/Plugins"
        LIBRARY DESTINATION "Data/SKSE/Plugins"
    )
    install(RUNTIME_DEPENDENCY_SET AnimatedLoadingScreensRuntimeDeps
        DESTINATION "Data/SKSE/Plugins"
        PRE_EXCLUDE_REGEXES "api-ms-" "ext-ms-"
        POST_EXCLUDE_REGEXES
            ".*[Ww]indows[\\\\/].*"
            ".*[Ss]ystem32[\\\\/].*"
    )
    install(FILES resources/AnimatedLoadingScreens.ini DESTINATION "Data/SKSE/Plugins")
    install(FILES resources/put_your_videos_here.txt DESTINATION "Data/SKSE/Plugins/AnimatedLoadingScreens/LoadingScreens")
    install(FILES resources/default_playlist.txt RENAME default.txt DESTINATION "Data/SKSE/Plugins/AnimatedLoadingScreens/Playlists")
    install(DIRECTORY DESTINATION "Data/SKSE/Plugins/AnimatedLoadingScreens/FFmpeg")

    if(MSVC)
        set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION "Data/SKSE/Plugins")
        include(InstallRequiredSystemLibraries)
    endif()
endif()

if(ALS_BUILD_TESTS)
    enable_testing()

    add_executable(als_tests
        src/Decode/FFmpegVideoDecoder.cpp
        tests/AspectRatioTests.cpp
        tests/ConfigTests.cpp
        tests/ControllerTests.cpp
        tests/FadeMathTests.cpp
        tests/MediaLibraryTests.cpp
        tests/PlaylistTests.cpp
        tests/SelectionTests.cpp
        tests/TestMain.cpp
    )

    target_link_libraries(als_tests PRIVATE ALS::Core)
    target_compile_definitions(als_tests PRIVATE ALS_TESTING=1 ALS_ENABLE_FFMPEG=0)

    if(WIN32)
        target_sources(als_tests PRIVATE src/Render/D3D11OverlayRenderer.cpp)
        target_link_libraries(als_tests PRIVATE d3d11 dxgi)
    endif()

    add_test(NAME AnimatedLoadingScreens.UnitTests COMMAND als_tests)
endif()
