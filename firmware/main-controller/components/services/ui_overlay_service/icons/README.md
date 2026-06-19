Overlay icon assets
===================

Editable source icons live in `assets/ui/icons/*.svg`. Runtime mask sources live
in `assets/ui/icons/28x28/*.pbm` as monochrome 28x28 PBM P1 masks. The firmware
uses the generated 1-bit masks from `generated/nb_ui_overlay_icons.h` and tints
them at draw time, avoiding PNG/SVG decoding and runtime allocation.

Recommended workflow:

1. Design the icon in SVG/Figma at `viewBox="0 0 24 24"`.
2. Export or trace it to a monochrome 28x28 PBM (`P1`) mask.
3. Regenerate the header:

```cmd
C:\Users\Tauser\AppData\Local\Python\pythoncore-3.14-64\python.exe tools\generate_overlay_icons.py
```

Keep icons simple, high contrast, and readable at 28x28. Animation/glow should
stay in `ui_overlay_service.cpp`; the asset should define only the silhouette.

Optional source override:

```cmd
C:\Users\Tauser\AppData\Local\Python\pythoncore-3.14-64\python.exe tools\generate_overlay_icons.py --source D:\Projetos\Noisebot\assets\ui\icons\28x28
```
