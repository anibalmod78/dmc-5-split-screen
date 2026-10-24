cmake_minimum_required(VERSION 3.15)
project(dmc5_splitscreen CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Incluir las cabeceras del proyecto, MinHook y REFramework
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/deps/minhook/include
    ${CMAKE_CURRENT_SOURCE_DIR}/deps/reframework/include
    ${CMAKE_CURRENT_SOURCE_DIR}/deps
)

# Recopilar fuentes de MinHook
file(GLOB MINHOOK_SOURCES "deps/minhook/src/*.c" "deps/minhook/src/hde/*.c")

# Recopilar los archivos de tu Mod
file(GLOB MOD_SOURCES "*.cpp" "*.hpp")

# Crear la DLL del Mod
add_library(dmc5_splitscreen SHARED ${MOD_SOURCES} ${MINHOOK_SOURCES})

target_compile_definitions(dmc5_splitscreen PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_link_libraries(dmc5_splitscreen PRIVATE xinput)
