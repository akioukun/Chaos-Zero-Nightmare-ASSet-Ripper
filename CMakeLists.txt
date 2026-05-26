cmake_minimum_required(VERSION 3.16)
project(ChaosZeroNightmareRipper LANGUAGES CXX)

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

add_executable(
    ${PROJECT_NAME}
    main.cpp
    DataPack.cpp
    SCTParser.cpp
    DBParser.cpp
    SCSPParser.cpp
    SpineDictionary.cpp
    SpineRenderer.cpp
    ${SPINE_CPP_SOURCES}
)

if(WIN32)
    target_sources(${PROJECT_NAME} PRIVATE "assets/appicon.rc")
endif()
target_include_directories(
    ${PROJECT_NAME}
    PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/libs"
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