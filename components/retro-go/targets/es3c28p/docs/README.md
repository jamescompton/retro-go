# LCDWIKI ES3C28P

2.8" ESP32-S3 display module (ILI9341V 320x240, FT6336G touch, 16MB flash, 8MB OPI PSRAM).

- Input: on-screen touch zones (see `RG_GAMEPAD_TOUCH_MAP`) and the BOOT button (menu).
- Storage: MicroSD in SDMMC 1-bit mode.
- Audio: ES8311 codec over I2S (driver in `drivers/audio/i2s.c`).
- Build: `python rg_tool.py --target es3c28p --port COM19 install`

## Status

Verified working on hardware (hand-tested by the author on one board, ESP-IDF 5.4.2):

- Display, microSD, FT6336 touch input and the BOOT button (menu).
- ES8311 audio output.
- Launcher, and games running from the enlarged app partitions (`env.py`).
- Touch overlay drawn in the launcher and in games, with Options entries for
  Touch buttons (on by default), Touch colour, Touch opacity and Touch labels.

Known issues / not yet tested:

- Bluetooth gamepad: NimBLE + `esp_hidh` code exists in `rg_ble.c` but is compiled out
  (`-DRG_ENABLE_BLE_GAMEPAD` is commented out in `components/retro-go/CMakeLists.txt`).
  A Shanwan Q36 was found by scanning but never finished connecting (`esp_hidh_dev_open`
  stalled); an Xbox One pad paired but dropped the link on button presses.
- USB controllers: not supported. The board's USB-C is power plus a USB-serial chip.
- Audio was once silent in Doom until settings were reset; cause unknown.
- Not every emulator core has been tested.
