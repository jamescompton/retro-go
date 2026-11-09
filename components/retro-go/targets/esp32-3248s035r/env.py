# Espressif chip in the device
IDF_TARGET = "esp32"
# This target uses standard ESP-IDF .img firmware images.
FW_FORMAT = "none"
# Each profile preserves the x50 app offsets and fits the board's 4MB flash.
profiles = {
    "core": "retro-core",
    "md": "gwenesis",
    "doom": "prboom-go",
}
profile = os.getenv("RG_3248_PROFILE", "core")
if profile not in profiles:
    raise ValueError("RG_3248_PROFILE must be core, md or doom")
DEFAULT_APPS = "launcher " + profiles[profile]
for app in PROJECT_APPS:
    PROJECT_APPS[app][2] = 0x170000
