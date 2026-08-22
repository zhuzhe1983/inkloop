# M5 PaperColor C151 adapter

Target metadata: ESP32-S3, 400×600 six-color ED2208 e-paper, PM1 power
controller, two WS2812 pixels on GPIO21, three buttons (GPIO10/GPIO9/GPIO1),
microphone, speaker, PSRAM and optional TF/SD storage.

This adapter is native ESP-IDF. It owns one internal I2C bus, the M5PM1 power
sequence, one shared SPI2 bus for ED2208 and TF, active-low buttons, two
GPIO21 WS2812 pixels, and the ES7210/ES8311 codec controls. ED2208 initialize
does not refresh the panel; only a complete validated 400×600 native-palette
frame can start a visible refresh.

Digital compilation and host protocol tests do not replace the C151 physical
gate. Do not wrap M5Unified or Arduino inside this directory.
