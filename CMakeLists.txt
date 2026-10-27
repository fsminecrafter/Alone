cmake_minimum_required(VERSION 3.16)
project(Alone VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(ALONE_BUILD_CLIENT "Build the game client (needs raylib, fetched automatically)" ON)
option(ALONE_BUILD_TESTS  "Build the backend unit tests" ON)
if(EMSCRIPTEN)
    # Web build (emcmake cmake ...): browser game only
    set(ALONE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
endif()

if(MSVC)
    add_compile_options(/W3 /utf-8)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX)
else()
    add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endif()

# ---------------------------------------------------------------------------
# Backend: all game rules and simulation. No graphics dependency, so it can
# be unit tested and later reused by a dedicated server or tools.
# ---------------------------------------------------------------------------
file(GLOB_RECURSE ALONE_CORE_SOURCES CONFIGURE_DEPENDS
    src/core/*.cpp src/world/*.cpp src/survival/*.cpp src/items/*.cpp src/game/*.cpp)
add_library(alone_core STATIC ${ALONE_CORE_SOURCES})
target_include_directories(alone_core PUBLIC src)

# ---------------------------------------------------------------------------
# Client: rendering, input, UI and audio on top of raylib.
# ---------------------------------------------------------------------------
if(ALONE_BUILD_CLIENT)
    include(FetchContent)
    find_package(raylib 5.5 QUIET)
    if(NOT raylib_FOUND)
        set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(BUILD_GAMES    OFF CACHE BOOL "" FORCE)
        set(CUSTOMIZE_BUILD OFF CACHE BOOL "" FORCE)
        if(EMSCRIPTEN)
            set(PLATFORM "Web" CACHE STRING "" FORCE)
            set(OPENGL_VERSION "ES 3.0" CACHE STRING "" FORCE)   # WebGL 2
        endif()
        FetchContent_Declare(raylib
            GIT_REPOSITORY https://github.com/raysan5/raylib.git
            GIT_TAG        5.5
            GIT_SHALLOW    TRUE)
        FetchContent_MakeAvailable(raylib)
    endif()

    file(GLOB_RECURSE ALONE_CLIENT_SOURCES CONFIGURE_DEPENDS src/client/*.cpp)
    add_executable(alone ${ALONE_CLIENT_SOURCES})
    target_link_libraries(alone PRIVATE alone_core raylib)
    if(NOT MSVC)
        target_compile_options(alone PRIVATE -Wno-missing-field-initializers)
    endif()
    if(WIN32)
        # No console window in release builds on Windows.
        if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
            set_target_properties(alone PROPERTIES WIN32_EXECUTABLE ON)
            if(MSVC)
                target_link_options(alone PRIVATE /ENTRY:mainCRTStartup)
            endif()
        endif()
        if(MINGW)
            target_link_options(alone PRIVATE -static -static-libgcc -static-libstdc++)
        endif()
    endif()

    if(EMSCRIPTEN)
        # alone.html + alone.js + alone.wasm + alone.data (the data folder, packed)
        set_target_properties(alone PROPERTIES SUFFIX ".html")
        target_link_options(alone PRIVATE
            -sUSE_GLFW=3 -sMIN_WEBGL_VERSION=2 -sMAX_WEBGL_VERSION=2
            -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=268435456 -sSTACK_SIZE=5242880
            -sFORCE_FILESYSTEM=1 -lidbfs.js
            -sEXPORTED_RUNTIME_METHODS=FS,IDBFS,addRunDependency,removeRunDependency,HEAPF32,HEAP32,HEAPU8,HEAP16,HEAP8
            --preload-file ${CMAKE_SOURCE_DIR}/data@/data
            --pre-js ${CMAKE_SOURCE_DIR}/web/pre.js
            --shell-file ${CMAKE_SOURCE_DIR}/web/shell.html)
        set_property(TARGET alone APPEND PROPERTY LINK_DEPENDS
            ${CMAKE_SOURCE_DIR}/web/shell.html ${CMAKE_SOURCE_DIR}/web/pre.js)
    else()
        # Ship the data folder next to the executable.
        add_custom_command(TARGET alone POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                    ${CMAKE_SOURCE_DIR}/data $<TARGET_FILE_DIR:alone>/data)
    endif()

    install(TARGETS alone RUNTIME DESTINATION .)
    install(DIRECTORY data DESTINATION .)
endif()

if(ALONE_BUILD_TESTS)
    enable_testing()
    file(GLOB ALONE_TEST_SOURCES CONFIGURE_DEPENDS tests/*.cpp)
    add_executable(alone_tests ${ALONE_TEST_SOURCES})
    target_link_libraries(alone_tests PRIVATE alone_core)
    target_compile_definitions(alone_tests PRIVATE ALONE_DATA_DIR="${CMAKE_SOURCE_DIR}/data")
    if(MINGW)
        target_link_options(alone_tests PRIVATE -static -static-libgcc -static-libstdc++)
    endif()
    add_test(NAME alone_tests COMMAND alone_tests)

    # Developer tool: top-down image of the generated world
    add_executable(alone_worlddump tools/worlddump/main.cpp)
    target_link_libraries(alone_worlddump PRIVATE alone_core)
endif()
