# Inkloop portable six-color renderer

This component is the hardware-independent renderer shared by future Inkloop
ESP-IDF board adapters. It was migrated byte-for-byte from the validated
`InkloopDisplayPower/ImageProcessing` implementation, then only its include
path was namespaced.

`reflectance-photo` retains the attribution recorded in the source for the
separately authorized PaperColor-Frame integration. The component emits RGB
palette pixels; a board-specific sink packs those pixels for its controller.
