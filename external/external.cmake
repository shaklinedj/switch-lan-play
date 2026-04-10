option(UV_LIBRARY "use installed libuv instead of building from source")
option(UVW_LIBRARY "use installed uvw instead of building from source")
option(UV_TERMUX_PATCH "apply libuv_termux.diff" ${OS_ANDROID})

if (UV_LIBRARY)
    find_package(Libuv REQUIRED)
    add_library(uv_a STATIC IMPORTED)
    set_target_properties(uv_a PROPERTIES
        IMPORTED_LOCATION ${LIBUV_LIBRARIES}
        INTERFACE_INCLUDE_DIRECTORIES ${LIBUV_INCLUDE_DIR}
    )
else()
    if (NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/libuv/CMakeLists.txt")
        message(STATUS "Installing libuv via submodule")
        execute_process(COMMAND git submodule update --init -- external/libuv
                        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                        RESULT_VARIABLE GIT_SUBMODULE_RESULT)
        if (NOT GIT_SUBMODULE_RESULT EQUAL 0)
            message(FATAL_ERROR "git submodule update failed for external/libuv (exit code ${GIT_SUBMODULE_RESULT}). "
                "Please run 'git submodule update --init' manually, or set -DUV_LIBRARY=ON to use a system-installed libuv.")
        endif()
        if (NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/libuv/CMakeLists.txt")
            message(FATAL_ERROR "external/libuv submodule is empty. "
                "Please run 'git submodule update --init' manually, or set -DUV_LIBRARY=ON to use a system-installed libuv.")
        endif()
    endif()
    add_subdirectory(external/libuv EXCLUDE_FROM_ALL)
    target_include_directories(uv_a INTERFACE external/libuv/include)
    if (UV_TERMUX_PATCH)
        message(STATUS "Apply libuv_termux.diff")
        execute_process(COMMAND git apply ../patch/libuv_termux.diff
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/external/libuv)
    else()
        execute_process(COMMAND git apply -R ../patch/libuv_termux.diff
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/external/libuv)
    endif()
endif()

if (UVW_LIBRARY)
    find_package(UVW REQUIRED)
    add_library(uvw STATIC IMPORTED)
    set_target_properties(uvw PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES ${LIBUVW_INCLUDE_DIR}
    )
else()
    if (NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/uvw/CMakeLists.txt")
        message(STATUS "Installing uvw via submodule")
        execute_process(COMMAND git submodule update --init -- external/uvw
                        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                        RESULT_VARIABLE GIT_SUBMODULE_RESULT)
        if (NOT GIT_SUBMODULE_RESULT EQUAL 0)
            message(FATAL_ERROR "git submodule update failed for external/uvw (exit code ${GIT_SUBMODULE_RESULT}). "
                "Please run 'git submodule update --init' manually, or set -DUVW_LIBRARY=ON to use a system-installed uvw.")
        endif()
        if (NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/uvw/CMakeLists.txt")
            message(FATAL_ERROR "external/uvw submodule is empty. "
                "Please run 'git submodule update --init' manually, or set -DUVW_LIBRARY=ON to use a system-installed uvw.")
        endif()
    endif()
    add_subdirectory(external/uvw EXCLUDE_FROM_ALL)
    include_directories(external/uvw/src)
endif()
