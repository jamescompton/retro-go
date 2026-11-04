# LCDWIKI ES3C28P

2.8" ESP32-S3 display module (ILI9341V 320x240, FT6336G touch, 16MB flash, 8MB OPI PSRAM).

- Input: on-screen touch zones (see `RG_GAMEPAD_TOUCH_MAP`) and the BOOT button (menu).
- Storage: MicroSD in SDMMC 1-bit mode.
- Audio: not supported yet (ES8311 codec needs I2C initialisation).
- Build: `python rg_tool.py --target es3c28p --port COM19 install`
