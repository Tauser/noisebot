cmake_minimum_required(VERSION 3.16)

# Inclui o toolchain e helpers do ESP-IDF antes de project()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(noisebot)

# Componentes do projeto (ordem não importa — CMake resolve dependências)
# Cada componente declara seus próprios REQUIRES em seu CMakeLists.txt
