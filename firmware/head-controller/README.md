# NoiseBot Head Controller

Firmware ESP-IDF da Freenove ESP32-S3-WROOM CAM N16R8.

Responsabilidades finais:

- display e LovyanGFX;
- renderização de face, gaze e overlays;
- touchscreen do display;
- câmera OV2640, preview e métricas leves;
- único microSD do produto;
- servidor de arquivos para o main-controller;
- enlace inter-MCU, watchdog e modo degradado local.

O scaffold inicial implementa somente boot, identidade e validação do contrato
compartilhado. Hardware real entra em fases independentes e reversíveis.

Build:

```powershell
cd firmware/head-controller
idf.py build
```
