include(FindPackageHandleStandardArgs)

set(_FFMPEG_HINTS)
if(DEFINED FFMPEG_ROOT)
    list(APPEND _FFMPEG_HINTS "${FFMPEG_ROOT}")
endif()
if(DEFINED ENV{FFMPEG_ROOT})
    list(APPEND _FFMPEG_HINTS "$ENV{FFMPEG_ROOT}")
endif()

set(_FFMPEG_COMPONENTS ${FFmpeg_FIND_COMPONENTS})
if(NOT _FFMPEG_COMPONENTS)
    set(_FFMPEG_COMPONENTS avformat avcodec avutil swscale)
endif()

foreach(_component IN LISTS _FFMPEG_COMPONENTS)
    string(TOUPPER "${_component}" _COMPONENT_UPPER)

    find_path(FFmpeg_${_component}_INCLUDE_DIR
        NAMES "lib${_component}/${_component}.h"
        HINTS ${_FFMPEG_HINTS}
        PATH_SUFFIXES include
    )

    find_library(FFmpeg_${_component}_LIBRARY
        NAMES ${_component} lib${_component}
        HINTS ${_FFMPEG_HINTS}
        PATH_SUFFIXES lib lib64
    )

    if(FFmpeg_${_component}_INCLUDE_DIR AND FFmpeg_${_component}_LIBRARY)
        set(FFmpeg_${_component}_FOUND TRUE)
        if(NOT TARGET FFmpeg::${_component})
            add_library(FFmpeg::${_component} UNKNOWN IMPORTED)
            set_target_properties(FFmpeg::${_component} PROPERTIES
                IMPORTED_LOCATION "${FFmpeg_${_component}_LIBRARY}"
                INTERFACE_INCLUDE_DIRECTORIES "${FFmpeg_${_component}_INCLUDE_DIR}"
            )
        endif()
    else()
        set(FFmpeg_${_component}_FOUND FALSE)
    endif()

    list(APPEND _FFMPEG_REQUIRED_VARS
        FFmpeg_${_component}_INCLUDE_DIR
        FFmpeg_${_component}_LIBRARY
    )
endforeach()

find_package_handle_standard_args(FFmpeg
    REQUIRED_VARS ${_FFMPEG_REQUIRED_VARS}
    HANDLE_COMPONENTS
)

