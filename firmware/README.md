# Firmwares do NoiseBot

Este diretório contém projetos ESP-IDF independentes.

- `main-controller/`: controlador principal Waveshare ESP32-S3 N32R16.
- `head-controller/`: controlador Freenove ESP32-S3 N16R8 para display,
  touchscreen, câmera e microSD.
- `shared/`: contratos binários compartilhados. Não contém lógica específica
  de placa.

Cada firmware possui seu próprio `CMakeLists.txt`, `sdkconfig.defaults`,
partições, build e ciclo de release. Nunca compartilhe diretórios `build/`,
`sdkconfig` gerado ou `managed_components/` entre os dois projetos.

Durante a migração, o `main-controller` ainda contém temporariamente display,
câmera, SD e LovyanGFX para preservar o baseline compilável. Esses componentes
serão removidos somente após suas facades remotas estarem validadas.
