cmake_minimum_required(VERSION 3.15)
project(dmc5_splitscreen CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Cabeceras del proyecto, MinHook y REFramework
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/deps/minhook/include
    ${CMAKE_CURRENT_SOURCE_DIR}/deps/reframework/include
    ${CMAKE_CURRENT_SOURCE_DIR}/deps
)

# Fuentes de MinHook
file(GLOB MINHOOK_SOURCES "deps/minhook/src/*.c" "deps/minhook/src/hde/*.c")

# Fuentes del mod. CONFIGURE_DEPENDS: si anades/quitas un archivo (p. ej. tracer.cpp)
# CMake lo detecta en la siguiente compilacion; sin esto se compila una DLL vieja.
file(GLOB MOD_SOURCES CONFIGURE_DEPENDS "*.cpp" "*.hpp")

add_library(dmc5_splitscreen SHARED ${MOD_SOURCES} ${MINHOOK_SOURCES})

target_compile_definitions(dmc5_splitscreen PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
)

if(MSVC)
    # Los fuentes estan en UTF-8; sin esto MSVC los interpreta con la pagina de codigos local.
    target_compile_options(dmc5_splitscreen PRIVATE /utf-8)
endif()

target_link_libraries(dmc5_splitscreen PRIVATE xinput)

# OPCIONAL: copia automatica de la DLL al juego tras compilar. Evita ejecutar una DLL
# vieja por olvidar copiarla. Ejemplo (en CMakeSettings.json o linea de comandos):
#   -DDMC5_DIR="E:/SteamLibrary/steamapps/common/Devil May Cry 5"
if(DMC5_DIR)
    add_custom_command(TARGET dmc5_splitscreen POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${DMC5_DIR}/reframework/plugins"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:dmc5_splitscreen>" "${DMC5_DIR}/reframework/plugins/"
        COMMENT "Copiando dmc5_splitscreen.dll a ${DMC5_DIR}/reframework/plugins")
endif()
