# This file is injected late into rg_tool.py, you can run arbitrary python code here
# For example override python variables or set environment variables with os.putenv

# Espressif chip in the device
IDF_TARGET = "esp32s3"
# .fw file format, if supported by the device
FW_FORMAT = "none"
# The BLE HID stack needs more room than the default app partitions
PROJECT_APPS["launcher"][2] = 1572864
PROJECT_APPS["retro-core"][2] = 1572864
PROJECT_APPS["gwenesis"][2] = 1572864
PROJECT_APPS["prboom-go"][2] = 1048576
PROJECT_APPS["fmsx"][2] = 786432
# Default apps to build when none is specified (comment to build all)
# DEFAULT_APPS = "launcher prboom-go"
