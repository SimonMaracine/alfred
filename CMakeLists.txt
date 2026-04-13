cmake_minimum_required(VERSION 3.20)

project(
    "alfred-syn-seq"
    VERSION "0.6.0"
    DESCRIPTION "A cross-platform audio synthesizer, sequencer and composition editor program"
    HOMEPAGE_URL "https://github.com/SimonMaracine/alfred-syn-seq"
    LANGUAGES CXX
)

include(cmake/options.cmake)

set(CACHE{CMAKE_BUILD_TYPE} TYPE STRING VALUE "Debug")
set(CACHE{CMAKE_CONFIGURATION_TYPES} TYPE STRING FORCE VALUE "Debug;Release")
set(BUILD_SHARED_LIBS OFF)

include(cmake/sanity_checks.cmake)
include(cmake/target_options.cmake)
include(cmake/ipo.cmake)
include(cmake/sdl.cmake)

add_subdirectory(lib)
add_subdirectory(app)

include(cmake/install.cmake)
include(cmake/package.cmake)
include(CPack)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)
set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT alfred_app)

message(STATUS "ALFRED_DISTRIBUTION: ${ALFRED_DISTRIBUTION}")
message(STATUS "ALFRED_ASAN: ${ALFRED_ASAN}")
message(STATUS "ALFRED_ASAN_THREAD: ${ALFRED_ASAN_THREAD}")
message(STATUS)
