Fontes de overlay importadas/adaptadas de `D:\Projetos\StackChan`.

- `MontserratSemiBold26.c`: fonte LVGL gerada a partir de
  `Montserrat-SemiBold.ttf` e usada pelo StackChan no launcher.
- `symbols.txt`: lista ASCII usada para gerar a fonte.

Adaptação NoiseBot:

- `ui_overlay_assets.h` centraliza a declaração da fonte, equivalente ao papel
  do `assets/assets.h` no StackChan;
- removido o campo `static_bitmap` do descritor `lv_font_t`, pois o shim LVGL
  embutido no LovyanGFX do NoiseBot não expõe esse campo;
- `MontserratPtBr16.c`: fonte LVGL gerada a partir de
  `Montserrat-SemiBold.ttf`, com ASCII + Latin-1 (`0x20-0x7F,0xA0-0xFF`), usada
  no balão de resposta para preservar UTF-8/acento sem normalização no server e
  sem desenho manual de diacríticos.
