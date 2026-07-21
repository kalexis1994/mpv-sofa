cmake_minimum_required(VERSION 3.20)
project(mpv-sofa VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

# Output directories — final runtime goes to dist/ (clean, shippable).
# CMake build/ keeps only its intermediate artefacts (CMakeFiles, CMakeCache).
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/dist)

# Options
option(USE_SYSTEM_GLFW "Use system-installed GLFW" OFF)
option(USE_SYSTEM_GLM "Use system-installed GLM" OFF)

# ---- Dependencies ----

# GLFW
if(USE_SYSTEM_GLFW)
    find_package(glfw3 3.3 REQUIRED)
else()
    set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    add_subdirectory(external/glfw)
endif()

# GLM
if(USE_SYSTEM_GLM)
    find_package(glm REQUIRED)
else()
    add_subdirectory(external/glm)
endif()

# GLAD (vendored)
add_library(glad STATIC
    external/glad/src/glad.c
)
target_include_directories(glad PUBLIC external/glad/include)

# Dear ImGui (vendored from submodule)
set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/external/imgui)
set(IMGUI_PATCHED_ROOT ${CMAKE_BINARY_DIR}/generated/imgui)
set(IMGUI_PATCHED_BACKEND_DIR ${IMGUI_PATCHED_ROOT}/backends)
set(IMGUI_HDR_PATCH ${CMAKE_SOURCE_DIR}/patches/imgui-opengl3-hdr.patch)
file(MAKE_DIRECTORY ${IMGUI_PATCHED_BACKEND_DIR})
configure_file(${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
               ${IMGUI_PATCHED_BACKEND_DIR}/imgui_impl_opengl3.cpp COPYONLY)
configure_file(${IMGUI_DIR}/backends/imgui_impl_opengl3.h
               ${IMGUI_PATCHED_BACKEND_DIR}/imgui_impl_opengl3.h COPYONLY)
find_package(Git REQUIRED)

# Give the staging tree its own git repository.  This is load-bearing, not
# hygiene: `git apply` run inside a repository resolves the patch's paths
# from that repository's root and silently ignores anything outside the
# current directory.  When the build directory sits inside the project
# checkout (`cmake -B build`, which is what CI does) git discovers the
# project repo, decides every path in the patch is out of scope, and exits
# 0 having changed nothing — so the pristine backend gets compiled and the
# HDR entry points don't exist.  GIT_CEILING_DIRECTORIES was meant to stop
# that walk but is matched literally, so it misses whenever CMake's
# "C:/..." spelling doesn't match the path form the git build uses.
# Making this directory a repository root stops discovery here, always.
if(NOT EXISTS ${IMGUI_PATCHED_ROOT}/.git)
    execute_process(COMMAND ${GIT_EXECUTABLE} init -q
                    WORKING_DIRECTORY ${IMGUI_PATCHED_ROOT}
                    RESULT_VARIABLE IMGUI_GIT_INIT_RESULT
                    OUTPUT_QUIET ERROR_QUIET)
    if(NOT IMGUI_GIT_INIT_RESULT EQUAL 0)
        message(FATAL_ERROR "Could not stage the ImGui patch tree at ${IMGUI_PATCHED_ROOT}")
    endif()
endif()

# --ignore-whitespace keeps this portable across git builds: the vendored
# checkout is CRLF on Windows (core.autocrlf) while the patch is LF, and
# only some builds reconcile the two — Git-for-Windows does, MSYS2's
# doesn't.
set(IMGUI_GIT_APPLY ${GIT_EXECUTABLE} apply --ignore-whitespace --whitespace=nowarn)
# Re-configuring must not fail on an already-patched copy: configure_file
# only refreshes the destination when the vendored source changed, so the
# generated tree can still be carrying the patch from an earlier configure.
# Probe with --reverse --check first and skip when it's already in.
execute_process(
    COMMAND ${IMGUI_GIT_APPLY} --reverse --check ${IMGUI_HDR_PATCH}
    WORKING_DIRECTORY ${IMGUI_PATCHED_ROOT}
    RESULT_VARIABLE IMGUI_HDR_ALREADY
    OUTPUT_QUIET ERROR_QUIET
)
if(IMGUI_HDR_ALREADY EQUAL 0)
    message(STATUS "ImGui HDR backend patch already applied")
else()
    execute_process(
        COMMAND ${IMGUI_GIT_APPLY} ${IMGUI_HDR_PATCH}
        WORKING_DIRECTORY ${IMGUI_PATCHED_ROOT}
        RESULT_VARIABLE IMGUI_HDR_PATCH_RESULT
        ERROR_VARIABLE IMGUI_HDR_PATCH_ERROR
    )
    if(NOT IMGUI_HDR_PATCH_RESULT EQUAL 0)
        message(FATAL_ERROR "Failed to apply ImGui HDR backend patch: ${IMGUI_HDR_PATCH_ERROR}")
    endif()
endif()

# Never take the patch step's word for it.  Every way this has failed so far
# ended with a silently unpatched backend and a compile error hundreds of
# lines later; checking for the entry point the patch adds turns that into
# an immediate, obvious failure.
file(READ ${IMGUI_PATCHED_BACKEND_DIR}/imgui_impl_opengl3.h IMGUI_PATCHED_HEADER)
string(FIND "${IMGUI_PATCHED_HEADER}" "ImGui_ImplOpenGL3_SetHdrColorDefaults"
       IMGUI_HDR_MARKER)
if(IMGUI_HDR_MARKER EQUAL -1)
    message(FATAL_ERROR
            "ImGui HDR backend patch reported success but the result is unpatched "
            "(${IMGUI_PATCHED_BACKEND_DIR}/imgui_impl_opengl3.h). Delete the build "
            "directory and re-run CMake.")
endif()
unset(IMGUI_PATCHED_HEADER)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${IMGUI_HDR_PATCH})
add_library(imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
    ${IMGUI_PATCHED_BACKEND_DIR}/imgui_impl_opengl3.cpp
)
target_include_directories(imgui PUBLIC
    ${IMGUI_DIR}
    ${IMGUI_PATCHED_BACKEND_DIR}
    ${IMGUI_DIR}/backends
)
target_link_libraries(imgui PUBLIC glfw)

# ImGuiFileDialog (in-app file browser, Blender/Godot-style)
set(IFD_DIR ${CMAKE_SOURCE_DIR}/external/ImGuiFileDialog)
add_library(imguifiledialog STATIC
    ${IFD_DIR}/ImGuiFileDialog.cpp
)
target_include_directories(imguifiledialog PUBLIC
    ${IFD_DIR}
    ${IFD_DIR}/dirent
)
target_link_libraries(imguifiledialog PUBLIC imgui)
# Enable the Places sidebar (drives + bookmarks).  USE_PLACES_DEVICES makes
# the dialog auto-populate "Devices" with every mounted volume on Windows
# (C:, D:, F:, …); USE_PLACES_BOOKMARKS adds a user-editable group.  PUBLIC
# so callers see the same compile-time view of the API.
target_compile_definitions(imguifiledialog PUBLIC
    USE_PLACES_FEATURE
    USE_PLACES_BOOKMARKS
    USE_PLACES_DEVICES
    PLACES_PANE_DEFAULT_SHOWN=true
)

# libmpv (must be installed on the system or provide path)
find_package(PkgConfig)
if(PkgConfig_FOUND)
    pkg_check_modules(MPV IMPORTED_TARGET mpv)
endif()

if(NOT MPV_FOUND)
    # Manual fallback: user sets MPV_INCLUDE_DIR and MPV_LIBRARY
    set(MPV_INCLUDE_DIR "" CACHE PATH "Path to mpv include directory (containing mpv/client.h)")
    set(MPV_LIBRARY "" CACHE FILEPATH "Path to mpv library file (mpv.lib or libmpv.a)")

    if(MPV_INCLUDE_DIR AND MPV_LIBRARY)
        add_library(PkgConfig::MPV IMPORTED INTERFACE)
        set_target_properties(PkgConfig::MPV PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${MPV_INCLUDE_DIR}"
            INTERFACE_LINK_LIBRARIES "${MPV_LIBRARY}"
        )
        set(MPV_FOUND TRUE)
    else()
        message(WARNING "libmpv not found. Set MPV_INCLUDE_DIR and MPV_LIBRARY manually.")
    endif()
endif()

# ---- Main Application ----

set(APP_SOURCES
    src/main.cpp
    src/app/Application.cpp
    src/app/Window.cpp
    src/core/MediaServer.cpp
    src/renderer/Renderer.cpp
    src/renderer/Direct3DHdrPresenter.cpp
    src/renderer/Camera.cpp
    src/renderer/Shader.cpp
    src/renderer/Primitives.cpp
    src/renderer/Picking.cpp
    src/ui/ImGuiLayer.cpp
    src/ui/ControlPanel.cpp
    src/ui/TransportBar.cpp
    src/ui/TrackPicker.cpp
    src/ui/PreferencesDialog.cpp
    src/audio/MpvPlayer.cpp
    src/audio/BinauralRenderer.cpp
    src/core/SharedState.c
    src/core/Settings.cpp
)

# WIN32: link against the GUI subsystem on Windows so a console window
# isn't spawned alongside the app on every launch.  CMake's WIN32 flag
# is silently ignored on other platforms.  An optional console can
# still be opened on demand via the --console command-line flag (see
# main.cpp), and launching from cmd / PowerShell auto-attaches to the
# parent console so logs flow back to the user's terminal.
add_executable(mpv-sofa WIN32 ${APP_SOURCES})

# Windows resources: application icon + version block.  The .rc pulls the
# icon in by name, so windres needs assets/icons on its include path; the
# version numbers come from project(VERSION) so there's one source of truth.
if(WIN32)
    enable_language(RC)
    # The version goes through a generated header rather than -D flags:
    # quoting a string macro survives neither CMake's nor windres's escaping.
    string(REPLACE "." "," MPV_SOFA_VER_COMMA "${PROJECT_VERSION}")
    configure_file(${CMAKE_SOURCE_DIR}/src/app/version.h.in
                   ${CMAKE_BINARY_DIR}/generated/version.h @ONLY)
    target_sources(mpv-sofa PRIVATE ${CMAKE_SOURCE_DIR}/src/app/mpv-sofa.rc)
    set_source_files_properties(${CMAKE_SOURCE_DIR}/src/app/mpv-sofa.rc PROPERTIES
        COMPILE_FLAGS
            "-I \"${CMAKE_SOURCE_DIR}/assets/icons\" -I \"${CMAKE_BINARY_DIR}/generated\"")
endif()

target_include_directories(mpv-sofa PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/external/IconFontCppHeaders
)

target_link_libraries(mpv-sofa PRIVATE
    glfw
    glad
    glm::glm
    imgui
    imguifiledialog
    mysofa
    z
)

if(MPV_FOUND)
    target_link_libraries(mpv-sofa PRIVATE PkgConfig::MPV)
    target_compile_definitions(mpv-sofa PRIVATE HAVE_MPV=1)
endif()

# Windows-specific
if(WIN32)
    # iphlpapi + ws2_32: MediaServer::lanAddress (adapter enumeration)
    # d3d11 + dxgi: HDR presentation via GL->D3D11 interop swapchain.
    target_link_libraries(mpv-sofa PRIVATE opengl32 iphlpapi ws2_32 d3d11 dxgi)
endif()

# Copy assets to build directory
add_custom_command(TARGET mpv-sofa POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_SOURCE_DIR}/assets
    $<TARGET_FILE_DIR:mpv-sofa>/assets
)
