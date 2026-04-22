cmake_minimum_required(VERSION 3.20)
project(tawny LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# --- Dependencies -----------------------------------------------------------

# GLFW
include(FetchContent)
find_package(glfw3 3.3 QUIET)
if(NOT glfw3_FOUND)
    FetchContent_Declare(
        glfw
        GIT_REPOSITORY https://github.com/glfw/glfw.git
        GIT_TAG        3.4
        GIT_SHALLOW    TRUE
    )
    set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(glfw)
endif()

# GLAD (vendored, generated for OpenGL 3.3 Core)
add_library(glad STATIC extern/glad/src/gl.c)
target_include_directories(glad PUBLIC extern/glad/include)

# doctest (vendored, header-only)
add_library(doctest INTERFACE)
target_include_directories(doctest INTERFACE extern/doctest/include)

# --- Tawny executable -------------------------------------------------------

file(GLOB_RECURSE TAWNY_SOURCES src/*.cpp src/*.h)
file(GLOB_RECURSE TEST_SOURCES  test/*.cpp test/*.h)

add_executable(tawny ${TAWNY_SOURCES} ${TEST_SOURCES})

target_include_directories(tawny PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/test
)

target_link_libraries(tawny PRIVATE glfw glad doctest)

target_compile_options(tawny PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Werror>
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX>
)

# --- Post-build: run tests ---------------------------------------------------

add_custom_command(TARGET tawny POST_BUILD
    COMMAND $<TARGET_FILE:tawny> --test --no-intro
    COMMENT "Running tests..."
    VERBATIM
)
