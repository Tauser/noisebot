# NoiseBot Main Controller

Firmware ESP-IDF do controlador principal Waveshare ESP32-S3 N32R16.

Responsabilidades finais:

- comportamento, persona, FSM e event bus;
- áudio, wake word, VAD, codec e bridge;
- Wi-Fi, API operacional e OTA;
- servos, `motion_safety`, LEDs, touch corporal e sensores;
- NVS de configuração e estado crítico;
- cliente do head-controller e cliente de armazenamento remoto.

Build:

```powershell
cd firmware/main-controller
idf.py build
```

Estado de migração: este projeto ainda mantém os serviços multimídia legados
até o head-controller substituir cada capability com rollback comprovado.
