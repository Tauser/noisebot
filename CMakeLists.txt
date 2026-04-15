cmake_minimum_required(VERSION 3.16)

# Inclui o toolchain e helpers do ESP-IDF antes de project()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Subdirectories com componentes de serviço (Layer 4-5) e safety (Layer 3).
# ESP-IDF só faz auto-discovery um nível abaixo de components/, então
# subdiretórios de categoria precisam ser declarados aqui.
list(APPEND EXTRA_COMPONENT_DIRS
    "components/services"
    "components/behavior"
)

project(noisebot)

# Componentes do projeto (ordem não importa — CMake resolve dependências)
# Cada componente declara seus próprios REQUIRES em seu CMakeLists.txt
