cmake_minimum_required(VERSION 3.15)
project(NukeCopyToPoints CXX)

# ---------------------------------------------------------------------------
# Nuke 14.1 NDK plugin: CopyToPoints (classic 3D system, GeoOp)
#
# Configure (Nuke 14 => Visual Studio 2019 toolchain, see NDK_NOTES 1.1):
#   cmake -G "Visual Studio 16 2019" -A x64 ^
#         -DNuke_DIR="C:/Program Files/Nuke14.1v8/cmake" -B build
#   cmake --build build --config Release
#   cmake --install build --config Release --prefix "%USERPROFILE%/.nuke"
#
# Nuke_DIR must point at the directory that CONTAINS NukeConfig.cmake
# (the "cmake/" subfolder of the install), not the install root.
# ---------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT DEFINED Nuke_DIR)
    set(Nuke_DIR "C:/Program Files/Nuke14.1v8/cmake" CACHE PATH "Directory containing NukeConfig.cmake")
endif()

# Several Nuke versions may be installed and register themselves in the
# CMake user package registry; never let that override -DNuke_DIR.
set(CMAKE_FIND_USE_PACKAGE_REGISTRY OFF)
find_package(Nuke REQUIRED NO_CMAKE_PACKAGE_REGISTRY)

# add_nuke_plugin() comes from NukeConfig.cmake: MODULE library, no "lib"
# prefix, links Nuke::NDK, defines NOMINMAX + _USE_MATH_DEFINES on Windows.
# One source file -> one DLL -> one Op::Description; the DLL name must match
# the Description name exactly.
option(BUILD_CLASSIC "Build the classic-3D plugins (CopyToPoints, MultiplyCf) for Nuke 14.1" ON)
option(BUILD_USD "Build the USD / new-3D plugin (CopyToPointsUSD) for Nuke 15+ (needs Nuke_DIR of a 15+ install)" OFF)

set(CTP_PLUGINS)
if(BUILD_CLASSIC)
    list(APPEND CTP_PLUGINS CopyToPoints MultiplyCf)
endif()
if(BUILD_USD)
    list(APPEND CTP_PLUGINS CopyToPointsUSD ParticlesToUSD VolumeToUSD)
endif()
foreach(_plugin IN LISTS CTP_PLUGINS)
    add_nuke_plugin(${_plugin} src/${_plugin}.cpp)
    set_target_properties(${_plugin} PROPERTIES OUTPUT_NAME "${_plugin}")
    if(MSVC)
        target_compile_definitions(${_plugin} PRIVATE
            WIN32_LEAN_AND_MEAN
            _CRT_SECURE_NO_WARNINGS
            # VS 2022 17.10+ STL: std::mutex has a constexpr constructor that needs a
            # matching (new) msvcp140.dll; Nuke ships an older runtime -> crash.
            _DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR
        )
        # /W3: DDK headers are noisy at /W4.  /MD: match Nuke's dynamic CRT.
        target_compile_options(${_plugin} PRIVATE /W3 /EHsc /MD /permissive- /Zc:__cplusplus)
    else()
        # Linux / macOS (gcc / clang): the brush polls the mouse through X11 (Linux)
        # or CoreGraphics (macOS) and draws with OpenGL.  Untested by the author on
        # these platforms - see BUILDING.md.
        target_compile_options(${_plugin} PRIVATE -Wall -Wno-unused-parameter)
        if(APPLE)
            target_link_libraries(${_plugin} PRIVATE "-framework OpenGL" "-framework ApplicationServices")
        elseif(UNIX)
            find_package(OpenGL)
            find_package(X11)
            if(OPENGL_FOUND)
                target_link_libraries(${_plugin} PRIVATE ${OPENGL_gl_LIBRARY})
            endif()
            if(X11_FOUND)
                target_include_directories(${_plugin} PRIVATE ${X11_INCLUDE_DIR})
                target_link_libraries(${_plugin} PRIVATE ${X11_LIBRARIES})
            endif()
        endif()
    endif()
endforeach()

# ---------------------------------------------------------------------------
# Install: ~/.nuke/CopyToPoints/{CopyToPoints.dll, menu.py, README.md}
# plus an idempotent pluginAddPath line appended to ~/.nuke/menu.py
# ---------------------------------------------------------------------------
# The version a build is FOR, read out of Nuke_DIR.  Both flavours need it: the
# NDK is not compatible across MINOR versions, so there is one folder per minor.
string(REGEX MATCH "Nuke([0-9]+)\.([0-9]+)" _nk "${Nuke_DIR}")
set(_nk_major "${CMAKE_MATCH_1}")
set(_nk_minor "${CMAKE_MATCH_2}")

if(BUILD_CLASSIC)
    # These used to go to classic/nuke14 whatever they were built against, on the
    # assumption that the classic 3D system was a Nuke 14 thing.  It is not: the
    # particle nodes and the classic 3D pipeline are still in 17.1, and a
    # CopyToPoints built there loads and renders downstream of a force field.  So
    # they are installed per minor, like the USD one.
    if(_nk_major STREQUAL "" OR _nk_minor STREQUAL "")
        message(FATAL_ERROR "could not read a Nuke version out of Nuke_DIR: ${Nuke_DIR}")
    endif()
    message(STATUS "CopyToPoints -> CopyToPoints/classic/nuke${_nk_major}.${_nk_minor}")
    install(TARGETS CopyToPoints MultiplyCf
            DESTINATION CopyToPoints/classic/nuke${_nk_major}.${_nk_minor})
endif()
if(BUILD_USD)
    # per-version subfolder: CopyToPoints/usd/nuke<major>/CopyToPointsUSD.dll
    # Per MINOR version.  The NDK is not compatible across minors: a 16.0 build
    # will not load in 16.1, and a 17.0 build will not load in 17.1 - Nuke says
    # "The specified procedure could not be found" and, in 17.1, that failure
    # aborts start-up entirely.  init.py prefers nuke<major>.<minor> and falls
    # back to nuke<major>, so folders installed by older versions still work.
    if(_nk_major STREQUAL "" OR _nk_minor STREQUAL "")
        message(FATAL_ERROR "could not read a Nuke version out of Nuke_DIR: ${Nuke_DIR}")
    endif()
    message(STATUS "CopyToPointsUSD -> CopyToPoints/usd/nuke${_nk_major}.${_nk_minor}")
    install(TARGETS CopyToPointsUSD ParticlesToUSD VolumeToUSD
            DESTINATION CopyToPoints/usd/nuke${_nk_major}.${_nk_minor})
endif()
install(FILES nuke/menu.py nuke/init.py DESTINATION CopyToPoints)
# the licence travels with the plugin, not just with the repository
install(FILES LICENSE THIRD_PARTY_NOTICES.md DESTINATION CopyToPoints)
# the node and menu icons: one folder, not a copy per Nuke version
install(DIRECTORY nuke/icons/ DESTINATION CopyToPoints/icons FILES_MATCHING PATTERN "*.png")
foreach(_doc IN ITEMS README.md)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_doc}")
        install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/${_doc}" DESTINATION CopyToPoints)
    endif()
endforeach()
install(SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/cmake/install_user_menu.cmake")
