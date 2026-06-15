if(NOT DEFINED ALS_RUNTIME_DLL_DESTINATION OR ALS_RUNTIME_DLL_DESTINATION STREQUAL "")
    message(FATAL_ERROR "ALS_RUNTIME_DLL_DESTINATION is required.")
endif()

if(NOT DEFINED ALS_RUNTIME_DLLS OR ALS_RUNTIME_DLLS STREQUAL "")
    return()
endif()

foreach(_dll IN LISTS ALS_RUNTIME_DLLS)
    if(_dll STREQUAL "")
        continue()
    endif()
    if(NOT EXISTS "${_dll}")
        message(FATAL_ERROR "Runtime DLL '${_dll}' does not exist.")
    endif()

    get_filename_component(_dll_name "${_dll}" NAME)
    file(COPY_FILE
        "${_dll}"
        "${ALS_RUNTIME_DLL_DESTINATION}/${_dll_name}"
        ONLY_IF_DIFFERENT)
endforeach()
