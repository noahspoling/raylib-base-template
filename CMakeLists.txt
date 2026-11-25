cmake_minimum_required(VERSION 3.27)
project(__PROJECT_NAME__ C CXX)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

get_filename_component(PROJECT_DIR_NAME "${CMAKE_CURRENT_SOURCE_DIR}" NAME)
set(GRAMARYE_APP_NAME "${PROJECT_NAME}")
if(GRAMARYE_APP_NAME STREQUAL "__PROJECT_NAME__")
    set(GRAMARYE_APP_NAME "${PROJECT_DIR_NAME}")
endif()

include(FetchContent)

set(GRAMARYE_TARGET "desktop" CACHE STRING "Build flavor: desktop, web, android, macos_gles_angle")
set_property(CACHE GRAMARYE_TARGET PROPERTY STRINGS desktop web android macos_gles_angle)

if(GRAMARYE_TARGET STREQUAL "web")
    set(BUILD_WEB ON CACHE BOOL "" FORCE)
elseif(GRAMARYE_TARGET STREQUAL "android")
    set(BUILD_WEB OFF CACHE BOOL "" FORCE)
elseif(GRAMARYE_TARGET STREQUAL "macos_gles_angle")
    if(NOT APPLE)
        message(FATAL_ERROR "GRAMARYE_TARGET=macos_gles_angle requires APPLE host toolchain.")
    endif()
    set(BUILD_WEB OFF CACHE BOOL "" FORCE)
else()
    set(BUILD_WEB OFF CACHE BOOL "" FORCE)
endif()

file(GLOB_RECURSE PROJECT_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c"
)

file(GLOB_RECURSE PROJECT_HEADERS
    "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h"
)

if(GRAMARYE_TARGET STREQUAL "desktop" AND NOT CMAKE_BUILD_TYPE MATCHES "^(Release|MinSizeRel)$")
    set(GRAMARYE_DEV_TOOLS_DEFAULT ON)
else()
    set(GRAMARYE_DEV_TOOLS_DEFAULT OFF)
endif()
option(GRAMARYE_DEV_TOOLS "Build dev-only tools such as the shader playground" ${GRAMARYE_DEV_TOOLS_DEFAULT})
if(NOT GRAMARYE_DEV_TOOLS)
    list(FILTER PROJECT_SOURCES EXCLUDE REGEX "/src/dev/")
    list(FILTER PROJECT_HEADERS EXCLUDE REGEX "/include/dev/")
endif()

option(GRAMARYE_GAME_THREADING "Enable libcore sync primitives (real locks)" OFF)
set(GRAMARYE_THREADING ${GRAMARYE_GAME_THREADING} CACHE BOOL "" FORCE)

set(BUILD_TESTS OFF CACHE BOOL "" FORCE)

if(EXISTS "${CMAKE_SOURCE_DIR}/libs/gramarye-libcore")
    set(FETCHCONTENT_SOURCE_DIR_GRAMARYE_LIBCORE "${CMAKE_SOURCE_DIR}/libs/gramarye-libcore" CACHE PATH "" FORCE)
endif()
FetchContent_Declare(
    gramarye_libcore
    GIT_REPOSITORY "https://github.com/noahspoling/gramarye-libcore.git"
    GIT_TAG "main"
    GIT_PROGRESS TRUE
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(gramarye_libcore)

if(EXISTS "${CMAKE_SOURCE_DIR}/libs/gramarye-ecs")
    set(FETCHCONTENT_SOURCE_DIR_GRAMARYE_ECS "${CMAKE_SOURCE_DIR}/libs/gramarye-ecs" CACHE PATH "" FORCE)
endif()
FetchContent_Declare(
    gramarye_ecs
    GIT_REPOSITORY "https://github.com/noahspoling/gramarye-ecs.git"
    GIT_TAG "main"
    GIT_PROGRESS TRUE
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(gramarye_ecs)

set(LUA_ENABLE_SHARED OFF CACHE BOOL "" FORCE)
set(LUA_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(LUA_BUILD_BINARY OFF CACHE BOOL "" FORCE)
set(LUA_BUILD_COMPILER OFF CACHE BOOL "" FORCE)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)
if(EXISTS "${CMAKE_SOURCE_DIR}/libs/lua")
    set(FETCHCONTENT_SOURCE_DIR_LUA "${CMAKE_SOURCE_DIR}/libs/lua" CACHE PATH "" FORCE)
endif()
FetchContent_Declare(
    lua
    GIT_REPOSITORY "https://github.com/walterschell/Lua.git"
    GIT_TAG "v5.4.7"
    GIT_PROGRESS TRUE
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(lua)

set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

if(GRAMARYE_TARGET STREQUAL "web")
    if(NOT EMSCRIPTEN)
        message(FATAL_ERROR "GRAMARYE_TARGET=web requires an Emscripten toolchain (use emcmake or CMakePresets web-wasm).")
    endif()
    set(PLATFORM Web CACHE STRING "" FORCE)
elseif(GRAMARYE_TARGET STREQUAL "android")
    if(NOT ANDROID)
        message(FATAL_ERROR "GRAMARYE_TARGET=android requires ANDROID=ON (NDK toolchain file). Use build_android.sh or android-ndk preset.")
    endif()
    set(PLATFORM Android CACHE STRING "" FORCE)
elseif(GRAMARYE_TARGET STREQUAL "macos_gles_angle")
    set(PLATFORM Desktop CACHE STRING "" FORCE)
    set(OPENGL_VERSION "ES 2.0" CACHE STRING "" FORCE)
else()
    set(PLATFORM Desktop CACHE STRING "" FORCE)
    set(OPENGL_VERSION "OFF" CACHE STRING "" FORCE)
endif()

if(EXISTS "${CMAKE_SOURCE_DIR}/libs/raylib")
    set(FETCHCONTENT_SOURCE_DIR_RAYLIB "${CMAKE_SOURCE_DIR}/libs/raylib" CACHE PATH "" FORCE)
endif()
FetchContent_Declare(
    raylib
    GIT_REPOSITORY "https://github.com/raysan5/raylib.git"
    GIT_TAG "master"
    GIT_PROGRESS TRUE
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(raylib)

set(GRAMARYE_UI_LUA ON CACHE BOOL "" FORCE)

if(EXISTS "${CMAKE_SOURCE_DIR}/libs/gramarye-ui")
    set(FETCHCONTENT_SOURCE_DIR_GRAMARYE_UI "${CMAKE_SOURCE_DIR}/libs/gramarye-ui" CACHE PATH "" FORCE)
endif()
FetchContent_Declare(
    gramarye_ui
    GIT_REPOSITORY "https://github.com/noahspoling/gramarye-ui.git"
    GIT_TAG "main"
    GIT_PROGRESS TRUE
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(gramarye_ui)

set(GRAMARYE_NATIVE_LIBS "" CACHE STRING "Extra libraries linked into the runtime (genre C libs)")

set(RUNTIME_TARGET ${GRAMARYE_APP_NAME}_runtime)
add_library(${RUNTIME_TARGET} STATIC
    ${PROJECT_SOURCES}
    ${PROJECT_HEADERS}
)

target_include_directories(${RUNTIME_TARGET} PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${raylib_SOURCE_DIR}/src
)

target_compile_definitions(${RUNTIME_TARGET} PUBLIC
    GRAMARYE_APP_NAME="${GRAMARYE_APP_NAME}"
)

if(GRAMARYE_DEV_TOOLS)
    target_compile_definitions(${RUNTIME_TARGET} PUBLIC
        GRAMARYE_DEV_TOOLS=1
        GRAMARYE_DEV_DIR="${CMAKE_CURRENT_SOURCE_DIR}/dev/"
        GRAMARYE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/"
    )
endif()

target_link_libraries(${RUNTIME_TARGET} PUBLIC
    gramarye-libcore
    gramarye-ecs
    raylib
    lua_static
    gramarye-ui
    ${GRAMARYE_NATIVE_LIBS}
)

set(APPS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/apps)

if(GRAMARYE_TARGET STREQUAL "android")
    add_library(${GRAMARYE_APP_NAME} SHARED ${APPS_DIR}/single/main.c)
    set(GRAMARYE_APP_TARGETS ${GRAMARYE_APP_NAME})
elseif(GRAMARYE_TARGET STREQUAL "web")
    add_executable(${GRAMARYE_APP_NAME} ${APPS_DIR}/single/main.c)
    set(GRAMARYE_APP_TARGETS ${GRAMARYE_APP_NAME})
else()
    add_executable(${GRAMARYE_APP_NAME}        ${APPS_DIR}/launcher/main.c)
    add_executable(${GRAMARYE_APP_NAME}_game   ${APPS_DIR}/game/main.c)
    add_executable(${GRAMARYE_APP_NAME}_editor ${APPS_DIR}/editor/main.c)
    set(GRAMARYE_APP_TARGETS
        ${GRAMARYE_APP_NAME} ${GRAMARYE_APP_NAME}_game ${GRAMARYE_APP_NAME}_editor)
endif()

foreach(app_target IN LISTS GRAMARYE_APP_TARGETS)
    target_include_directories(${app_target} PRIVATE ${APPS_DIR})
    target_link_libraries(${app_target} PRIVATE ${RUNTIME_TARGET})
endforeach()

if(GRAMARYE_TARGET STREQUAL "web")
    set_target_properties(${GRAMARYE_APP_NAME} PROPERTIES SUFFIX ".html")
    target_link_options(${GRAMARYE_APP_NAME} PRIVATE
        "-sALLOW_MEMORY_GROWTH=1"
        "--preload-file=${CMAKE_CURRENT_SOURCE_DIR}/packages@packages"
        "--shell-file=${CMAKE_CURRENT_SOURCE_DIR}/web/shell.html"
    )
elseif(NOT GRAMARYE_TARGET STREQUAL "android")
    set(STAGE_DIR ${CMAKE_CURRENT_BINARY_DIR})
    if(WIN32)
        add_custom_target(${GRAMARYE_APP_NAME}_stage ALL
            COMMAND ${CMAKE_COMMAND} -E rm -rf "${STAGE_DIR}/packages"
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_CURRENT_SOURCE_DIR}/packages" "${STAGE_DIR}/packages"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${STAGE_DIR}/mods"
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_CURRENT_SOURCE_DIR}/mods" "${STAGE_DIR}/mods"
        )
    else()
        add_custom_target(${GRAMARYE_APP_NAME}_stage ALL
            COMMAND ${CMAKE_COMMAND} -E rm -rf "${STAGE_DIR}/assets"
            COMMAND ${CMAKE_COMMAND} -E rm -rf "${STAGE_DIR}/packages"
            COMMAND ${CMAKE_COMMAND} -E create_symlink
                "${CMAKE_CURRENT_SOURCE_DIR}/packages" "${STAGE_DIR}/packages"
            COMMAND ${CMAKE_COMMAND} -E rm -rf "${STAGE_DIR}/mods"
            COMMAND ${CMAKE_COMMAND} -E create_symlink
                "${CMAKE_CURRENT_SOURCE_DIR}/mods" "${STAGE_DIR}/mods"
        )
    endif()
    foreach(app_target IN LISTS GRAMARYE_APP_TARGETS)
        add_dependencies(${app_target} ${GRAMARYE_APP_NAME}_stage)
    endforeach()
endif()

if(GRAMARYE_TARGET STREQUAL "android")
    target_link_options(${GRAMARYE_APP_NAME} PRIVATE
        "-u" "ANativeActivity_onCreate"
        "-Wl,-z,max-page-size=16384"
    )
endif()
