# Espressif chip in the device
IDF_TARGET = "esp32"
# This target uses standard ESP-IDF .img firmware images.
FW_FORMAT = "none"
# Build only apps that fit in the board's 4MB flash.
DEFAULT_APPS = "launcher retro-core"
# Classic HID increases app sizes; omit Doom to retain networking within 4MB.
PROJECT_APPS["launcher"][2] = 1507328
PROJECT_APPS["retro-core"][2] = 1507328
