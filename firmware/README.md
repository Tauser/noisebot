# NoiseBot Firmware

This folder mirrors StackChan's top-level `firmware/` boundary.

The current ESP-IDF project still lives at the repository root to avoid breaking
existing `idf.py`, CMake, sdkconfig, component and build assumptions:

```text
components/
main/
CMakeLists.txt
sdkconfig.defaults
partitions.csv
managed_components/
```

Migration rule: move the ESP-IDF project into this folder only as a dedicated
refactor with a clean build before and after. Until then, treat this folder as
the architectural boundary and the root ESP-IDF files as the active firmware.

