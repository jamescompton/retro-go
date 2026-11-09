# Espressif chip in the device
IDF_TARGET = "esp32"
# This target uses standard ESP-IDF .img firmware images.
FW_FORMAT = "none"
# The 16-bit set needs three app slots; the other profiles retain x50 offsets.
profiles = {
    "8bit": "retro-core",
    "16bit": "retro-core gwenesis",
    "core": "retro-core",
    "md": "gwenesis",
    "doom": "prboom-go",
}
profile = os.getenv("RG_3248_PROFILE", "8bit")
if profile not in profiles:
    raise ValueError("RG_3248_PROFILE must be 8bit, 16bit, core, md or doom")
emulator_set = {"8bit": 1, "16bit": 2}.get(profile, 0)
DEFAULT_APPS = "launcher " + profiles[profile]
for app in PROJECT_APPS:
    PROJECT_APPS[app][2] = 0x170000
if profile == "16bit":
    PROJECT_APPS["launcher"][2] = 0x170000
    PROJECT_APPS["retro-core"][2] = 0x110000
    PROJECT_APPS["gwenesis"][2] = 0x170000
