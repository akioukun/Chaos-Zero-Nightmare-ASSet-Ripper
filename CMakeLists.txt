cmake_minimum_required(VERSION 3.16)
project(ChaosZeroNightmareRipper LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(SDL2 CONFIG REQUIRED)
find_package(SDL2_image CONFIG REQUIRED)
find_package(GLEW REQUIRED)
find_package(OpenGL REQUIRED)
find_package(Threads REQUIRED)

add_library(astc-encoder::astcenc-static STATIC IMPORTED)
set_target_properties(astc-encoder::astcenc-static
    PROPERTIES
    IMPORTED_LOCATION "${CMAKE_CURRENT_SOURCE_DIR}/libs/astc/astcenc-native-static.lib"

    INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/libs/astc"
)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

file(GLOB SPINE_CPP_SOURCES "libs/spine-cpp/src/spine/*.cpp")

# Sources shared by the GUI and the headless CLI
set(CZN_CORE_SOURCES
    archive/DataPack.cpp
    archive/ArchiveBase.cpp
    archive/SSRArchive.cpp
    archive/CompositeArchive.cpp
    parsers/SCTParser.cpp
    parsers/DBParser.cpp
    parsers/SCSPParser.cpp
    libs/zstd/zstddeclib.c
)

set(CZN_CORE_INCLUDE_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/core"
    "${CMAKE_CURRENT_SOURCE_DIR}/archive"
    "${CMAKE_CURRENT_SOURCE_DIR}/parsers"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs"
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/zstd"
)

# Compiled once and reused by both executables rather than built separately for each.
add_library(czn_core OBJECT ${CZN_CORE_SOURCES})
target_include_directories(czn_core PUBLIC ${CZN_CORE_INCLUDE_DIRS})
target_link_libraries(czn_core PUBLIC astc-encoder::astcenc-static Threads::Threads)

# GUI
add_executable(
    ${PROJECT_NAME}
    main.cpp
    $<TARGET_OBJECTS:czn_core>
    parsers/SpineDictionary.cpp
    parsers/SpineRenderer.cpp
    ${SPINE_CPP_SOURCES}
)

if(WIN32)
    target_sources(${PROJECT_NAME} PRIVATE "assets/appicon.rc")
endif()
target_include_directories(
    ${PROJECT_NAME}
    PRIVATE
    ${CZN_CORE_INCLUDE_DIRS}
    "${CMAKE_CURRENT_SOURCE_DIR}/libs/spine-cpp/include"
    ${SDL2_INCLUDE_DIRS}
    ${GLEW_INCLUDE_DIRS}
    ${OPENGL_INCLUDE_DIR}
)

target_link_libraries(${PROJECT_NAME}
    PRIVATE
    SDL2::SDL2
    SDL2::SDL2main
    SDL2_image::SDL2_image
    GLEW::GLEW
    OpenGL::GL
    astc-encoder::astcenc-static
    Threads::Threads
)

# Headless CLI
add_executable(ChaosZeroNightmareRipper-CLI cli/main_cli.cpp)

target_link_libraries(ChaosZeroNightmareRipper-CLI PRIVATE czn_core)

if(WIN32)
    # CommandLineToArgvW, used to read the command line as UTF-8
    target_link_libraries(ChaosZeroNightmareRipper-CLI PRIVATE Shell32)
    target_sources(ChaosZeroNightmareRipper-CLI PRIVATE "assets/appicon.rc")
endif()
