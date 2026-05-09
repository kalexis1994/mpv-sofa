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
add_library(imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui PUBLIC
    ${IMGUI_DIR}
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
    src/renderer/Renderer.cpp
    src/renderer/Camera.cpp
    src/renderer/Shader.cpp
    src/renderer/Primitives.cpp
    src/renderer/Picking.cpp
    src/ui/ImGuiLayer.cpp
    src/ui/ControlPanel.cpp
    src/ui/TransportBar.cpp
    src/audio/MpvPlayer.cpp
    src/core/SharedState.c
    src/core/Settings.cpp
)

add_executable(mpv-sofa ${APP_SOURCES})

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
)

if(MPV_FOUND)
    target_link_libraries(mpv-sofa PRIVATE PkgConfig::MPV)
    target_compile_definitions(mpv-sofa PRIVATE HAVE_MPV=1)
endif()

# Windows-specific
if(WIN32)
    target_link_libraries(mpv-sofa PRIVATE opengl32)
endif()

# Copy assets to build directory
add_custom_command(TARGET mpv-sofa POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_SOURCE_DIR}/assets
    $<TARGET_FILE_DIR:mpv-sofa>/assets
)
