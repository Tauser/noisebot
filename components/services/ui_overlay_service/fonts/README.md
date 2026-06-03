Fontes de overlay importadas/adaptadas de `D:\Projetos\StackChan`.

- `MontserratSemiBold26.c`: fonte LVGL gerada a partir de
  `Montserrat-SemiBold.ttf` e usada pelo StackChan no launcher.
- `symbols.txt`: lista ASCII usada para gerar a fonte.

Adaptação NoiseBot:

- removido o campo `static_bitmap` do descritor `lv_font_t`, pois o shim LVGL
  embutido no LovyanGFX do NoiseBot não expõe esse campo;
- uso limitado a títulos/destaques de overlay. Texto livre continua em fontes
  Montserrat nativas do LovyanGFX para evitar perda de acentos em português.
