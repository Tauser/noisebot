cmake_minimum_required(VERSION 3.16)

# Inclui o toolchain e helpers do ESP-IDF antes de project()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Subdirectories com componentes de serviço (Layer 4-5) e safety (Layer 3).
# ESP-IDF só faz auto-discovery um nível abaixo de components/, então
# subdiretórios de categoria precisam ser declarados aqui.
list(APPEND EXTRA_COMPONENT_DIRS
    "components/services"
    "components/behavior"
    "components/persona"
)

project(noisebot)

# Workaround: ICE do GCC (segfault interno) em esp_lcd_panel_rgb.c com -O2.
# Bug conhecido no toolchain Xtensa ao otimizar rgb_panel_draw_bitmap.
# -fno-ipa-sra desabilita a otimização IPA SRA que dispara o ICE.
idf_build_set_property(COMPILE_OPTIONS "-fno-ipa-sra" APPEND)

# Componentes do projeto (ordem não importa — CMake resolve dependências)
# Cada componente declara seus próprios REQUIRES em seu CMakeLists.txt
