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

option(GRAMARYE_GAME_THREADING "Enable libcore sync primitives (real locks)" OFF)
set(GRAMARYE_THREADING ${GRAMARYE_GAME_THREADING} CACHE BOOL "" FORCE)

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

set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
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

if(GRAMARYE_TARGET STREQUAL "android")
    add_library(${GRAMARYE_APP_NAME} SHARED
        ${PROJECT_SOURCES}
        ${PROJECT_HEADERS}
    )
else()
    add_executable(${GRAMARYE_APP_NAME}
        ${PROJECT_SOURCES}
        ${PROJECT_HEADERS}
    )
endif()

target_include_directories(${GRAMARYE_APP_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${raylib_SOURCE_DIR}/src
)

target_link_libraries(${GRAMARYE_APP_NAME} PRIVATE
    gramarye-libcore
    gramarye-ecs
    raylib
    lua_static
    gramarye-ui
)

if(GRAMARYE_TARGET STREQUAL "web")
    set_target_properties(${GRAMARYE_APP_NAME} PROPERTIES SUFFIX ".html")
    target_link_options(${GRAMARYE_APP_NAME} PRIVATE
        "-sALLOW_MEMORY_GROWTH=1"
        "--preload-file=${CMAKE_CURRENT_SOURCE_DIR}/assets@assets"
        "--shell-file=${CMAKE_CURRENT_SOURCE_DIR}/web/shell.html"
    )
elseif(NOT GRAMARYE_TARGET STREQUAL "android")
    if(WIN32)
        add_custom_command(TARGET ${GRAMARYE_APP_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_CURRENT_SOURCE_DIR}/assets"
                "$<TARGET_FILE_DIR:${GRAMARYE_APP_NAME}>/assets"
        )
    else()
        add_custom_command(TARGET ${GRAMARYE_APP_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E rm -rf
                "$<TARGET_FILE_DIR:${GRAMARYE_APP_NAME}>/assets"
            COMMAND ${CMAKE_COMMAND} -E create_symlink
                "${CMAKE_CURRENT_SOURCE_DIR}/assets"
                "$<TARGET_FILE_DIR:${GRAMARYE_APP_NAME}>/assets"
        )
    endif()
endif()

if(GRAMARYE_TARGET STREQUAL "android")
    target_link_options(${GRAMARYE_APP_NAME} PRIVATE
        "-u" "ANativeActivity_onCreate"
        "-Wl,-z,max-page-size=16384"
    )
endif()
