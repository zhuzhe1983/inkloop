# Inkloop portable six-color renderer

This component is the hardware-independent renderer shared by future Inkloop
ESP-IDF board adapters. It was migrated byte-for-byte from the validated
`InkloopDisplayPower/ImageProcessing` implementation, then only its include
path was namespaced.

`reflectance-photo` retains the attribution recorded in the source for the
separately authorized PaperColor-Frame integration. The component emits RGB
palette pixels; a board-specific sink packs those pixels for its controller.

`official-quality` is a bounded streaming port of the RGB-pair quantizer used
by M5GFX 0.2.27 `Panel_ED2208` in `epd_quality` mode. M5GFX/LovyanGFX is
distributed under its FreeBSD licence; the original source is
<https://github.com/lovyan03/LovyanGFX>. The port retains the official
two-pixel joint colour metric and dither level without importing the Arduino
or M5GFX runtime into ESP-IDF.
