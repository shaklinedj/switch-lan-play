include(FetchContent)

option(UV_LIBRARY "use installed libuv instead of building from source")
option(UVW_LIBRARY "use installed uvw instead of building from source")
option(UV_TERMUX_PATCH "apply libuv_termux.diff" ${OS_ANDROID})

# Pinned versions for FetchContent fallback
# These versions match the original submodule commits
set(LIBUV_FETCH_TAG "v1.24.1" CACHE STRING "libuv version to fetch if submodule unavailable")
set(UVW_FETCH_TAG "v1.12.0_libuv-v1.24" CACHE STRING "uvw version to fetch if submodule unavailable")

# Note: CMAKE_POLICY_VERSION_MINIMUM must be set via command line or CMakePresets.json
# to allow older dependencies with cmake_minimum_required(VERSION 3.0-3.4) to work
# with CMake 3.27+ which removed support for cmake_minimum_required < 3.5.
# Use: cmake --preset default  OR  cmake -DCMAKE_POLICY_VERSION_MINIMUM=3.5

# Disable tests in dependencies to avoid potential build issues
set(BUILD_TESTING OFF CACHE BOOL "Disable testing in dependencies" FORCE)
set(LIBUV_BUILD_TESTS OFF CACHE BOOL "Disable libuv tests" FORCE)

# Helper function to initialize submodule or fetch from GitHub
# Sets ${NAME}_FETCHED and ${NAME}_SOURCE_DIR in parent scope
function(init_or_fetch_dependency NAME SUBMODULE_PATH FETCH_URL FETCH_TAG)
    if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${SUBMODULE_PATH}/CMakeLists.txt")
        message(STATUS "${NAME} submodule already present")
        set(${NAME}_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/${SUBMODULE_PATH}" PARENT_SCOPE)
        set(${NAME}_FETCHED FALSE PARENT_SCOPE)
    else()
        # Try git submodule first
        message(STATUS "Initializing ${NAME} via git submodule...")
        execute_process(COMMAND git submodule update --init -- ${SUBMODULE_PATH}
                        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                        RESULT_VARIABLE GIT_SUBMODULE_RESULT
                        ERROR_VARIABLE GIT_SUBMODULE_ERROR
                        OUTPUT_QUIET)
        if (GIT_SUBMODULE_RESULT EQUAL 0 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${SUBMODULE_PATH}/CMakeLists.txt")
            message(STATUS "${NAME} initialized via git submodule")
            set(${NAME}_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/${SUBMODULE_PATH}" PARENT_SCOPE)
            set(${NAME}_FETCHED FALSE PARENT_SCOPE)
        else()
            if (GIT_SUBMODULE_ERROR)
                message(STATUS "Git submodule failed: ${GIT_SUBMODULE_ERROR}")
            endif()
            # Fallback to FetchContent (only declare, don't populate yet)
            message(STATUS "Git submodule unavailable, fetching ${NAME} from ${FETCH_URL} (${FETCH_TAG})...")
            FetchContent_Declare(${NAME}
                GIT_REPOSITORY ${FETCH_URL}
                GIT_TAG ${FETCH_TAG}
                GIT_SHALLOW TRUE
            )
            set(${NAME}_FETCHED TRUE PARENT_SCOPE)
        endif()
    endif()
endfunction()

if (UV_LIBRARY)
    find_package(Libuv REQUIRED)
    add_library(uv_a STATIC IMPORTED)
    set_target_properties(uv_a PROPERTIES
        IMPORTED_LOCATION ${LIBUV_LIBRARIES}
        INTERFACE_INCLUDE_DIRECTORIES ${LIBUV_INCLUDE_DIR}
    )
else()
    init_or_fetch_dependency(libuv "external/libuv" "https://github.com/libuv/libuv.git" "${LIBUV_FETCH_TAG}")
    if (libuv_FETCHED)
        FetchContent_MakeAvailable(libuv)
        FetchContent_GetProperties(libuv SOURCE_DIR libuv_SOURCE_DIR)
        # libuv v1.24.x sets include directories as PRIVATE, we need INTERFACE for dependents
        target_include_directories(uv_a INTERFACE $<BUILD_INTERFACE:${libuv_SOURCE_DIR}/include>)
    else()
        add_subdirectory(external/libuv EXCLUDE_FROM_ALL)
        target_include_directories(uv_a INTERFACE external/libuv/include)
        if (UV_TERMUX_PATCH)
            message(STATUS "Apply libuv_termux.diff")
            execute_process(COMMAND git apply ../patch/libuv_termux.diff
                WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/external/libuv)
        endif()
    endif()
endif()

if (UVW_LIBRARY)
    find_package(UVW REQUIRED)
    add_library(uvw STATIC IMPORTED)
    set_target_properties(uvw PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES ${LIBUVW_INCLUDE_DIR}
    )
else()
    init_or_fetch_dependency(uvw "external/uvw" "https://github.com/skypjack/uvw.git" "${UVW_FETCH_TAG}")
    if (uvw_FETCHED)
        FetchContent_MakeAvailable(uvw)
        FetchContent_GetProperties(uvw SOURCE_DIR uvw_SOURCE_DIR)
        include_directories(${uvw_SOURCE_DIR}/src)
    else()
        add_subdirectory(external/uvw EXCLUDE_FROM_ALL)
        include_directories(external/uvw/src)
    endif()
endif()
