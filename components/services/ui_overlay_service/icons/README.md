Overlay icon assets
===================

Original design files live in `source_svg/*.svg`. Firmware-ready source icons
live in `source/*.pbm` as monochrome 24x24 masks. The firmware uses the
generated 1-bit masks from `generated/nb_ui_overlay_icons.h` and tints them at
draw time, avoiding PNG/SVG decoding and runtime allocation.

Recommended workflow:

1. Design the icon in SVG/Figma at 24x24 and save it under `source_svg/`.
2. Export or trace it to a monochrome PBM (`P1`) mask under `source/`.
3. Regenerate the header:

```cmd
C:\Users\Tauser\AppData\Local\Python\pythoncore-3.14-64\python.exe tools\generate_overlay_icons.py
```

Keep icons simple, high contrast, and readable at 24x24. Animation/glow should
stay in `ui_overlay_service.cpp`; the asset should define only the silhouette.
