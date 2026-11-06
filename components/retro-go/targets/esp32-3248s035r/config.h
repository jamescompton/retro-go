// Target definition
#define RG_TARGET_NAME             "ESP32-3248S035R"
#define RG_DEFAULT_STARTUP_MODE     1

// Enable only for experimental firmware; overclocking can disrupt Bluetooth.
#ifndef RG_ENABLE_DEV_MODE
#define RG_ENABLE_DEV_MODE         0
#endif

// Temporary, once-per-second audio/display/input telemetry for hardware testing.
#ifndef RG_RUNTIME_DIAGNOSTICS
#define RG_RUNTIME_DIAGNOSTICS     1
#endif

// Storage
#define RG_STORAGE_ROOT             "/sd"
#define RG_STORAGE_SDSPI_HOST       SPI3_HOST
#define RG_STORAGE_SDSPI_SPEED      SDMMC_FREQ_DEFAULT

// Audio
#define RG_AUDIO_USE_INT_DAC        2
#define RG_AUDIO_USE_EXT_DAC        0
#define RG_AUDIO_DAC_SOFT_MUTE      1
#define RG_AUDIO_DMA_BUFFER_COUNT  8

// Video
#define RG_SCREEN_DRIVER            2
#define RG_SCREEN_HOST              SPI2_HOST
#define RG_SCREEN_SPEED             SPI_MASTER_FREQ_80M  // GPIO12-15 are native HSPI IOMUX pins; verified clean on hardware (x38)
#define RG_SCREEN_BACKLIGHT         1
#define RG_SCREEN_WIDTH             480
#define RG_SCREEN_HEIGHT            320
#define RG_SCREEN_ROTATION          1
#define RG_SCREEN_RGB_BGR           1
#define RG_SCREEN_VISIBLE_AREA      {0, 0, 0, 0}
#define RG_SCREEN_SAFE_AREA         {0, 0, 0, 0}
#define RG_GBC_SKIP_BUSY_VIDEO      1
#define RG_SMS_SKIP_BUSY_VIDEO      1
#define RG_NES_SKIP_BUSY_VIDEO      1
#define RG_PCE_SKIP_BUSY_VIDEO      1
#define RG_SNES_SKIP_BUSY_VIDEO     1
#define RG_LYNX_SKIP_BUSY_VIDEO     1
// Q36 labels: A bottom, B right. NES games use right = A, bottom = B.
#define RG_NES_BT_SWAP_AB           1
// SNES default keymap uses X/Y/L/R directly instead of Menu combos.
#define RG_SNES_FULL_PAD_KEYMAP     1
// SNES is CPU-bound here; skip sample interpolation for emulation headroom.
// (16 kHz was tried in x43: the internal DAC drained only ~9.7k samples/s and capped SNES at 30 fps.)
#define RG_SNES_AUDIO_INTERPOLATION 0
// Don't lower RG_SNES_AUDIO_SAMPLE_RATE: at 16 kHz the internal DAC reports 16000 Hz but drains only ~9.8k samples/s (x43, x48).
// Mix SNES audio on core 1 (core 0 runs the emulator and Bluetooth).
#define RG_SNES_AUDIO_TASK          1
// Mix one 60/50 Hz frame of audio per emulated frame instead of a fixed 1/50 s.
#define RG_SNES_AUDIO_FRAME_LENGTH  1
#define RG_SCREEN_INIT()                                                                                         \
    rg_usleep(120 * 1000);                                                                                       \
    ILI9341_CMD(0xF0, 0xC3);                                                                                     \
    ILI9341_CMD(0xF0, 0x96);                                                                                     \
    ILI9341_CMD(0x3A, 0x55);                                                                                     \
    ILI9341_CMD(0xB4, 0x01);                                                                                     \
    ILI9341_CMD(0xB6, 0x80, 0x02, 0x3B);                                                                         \
    ILI9341_CMD(0xE8, 0x40, 0x8A, 0x00, 0x00, 0x29, 0x19, 0xA5, 0x33);                                           \
    ILI9341_CMD(0xC1, 0x06);                                                                                     \
    ILI9341_CMD(0xC2, 0xA7);                                                                                     \
    ILI9341_CMD(0xC5, 0x18);                                                                                     \
    ILI9341_CMD(0xE0, 0xF0, 0x09, 0x0B, 0x06, 0x04, 0x15, 0x2F, 0x54, 0x42, 0x3C, 0x17, 0x14, 0x18, 0x1B);     \
    ILI9341_CMD(0xE1, 0xE0, 0x09, 0x0B, 0x06, 0x04, 0x03, 0x2B, 0x43, 0x42, 0x3B, 0x16, 0x14, 0x17, 0x1B);     \
    ILI9341_CMD(0xF0, 0x3C);                                                                                     \
    ILI9341_CMD(0xF0, 0x69);                                                                                     \
    rg_usleep(120 * 1000);

// Q36 in Xbox mode: A=0 B=1 X=3 Y=4 L1=6 R1=7 Start=11; bits 2, 5 and 8-10 are unused.
// L2/R2 are analog triggers (Simulation Brake/Accelerator), read through the trigger keys below.
#define RG_GAMEPAD_HID_BUTTON_MAP { \
    RG_KEY_A, RG_KEY_B, RG_KEY_NONE, RG_KEY_X, RG_KEY_Y, RG_KEY_NONE, \
    RG_KEY_L, RG_KEY_R, RG_KEY_NONE, RG_KEY_NONE, \
    RG_KEY_NONE, RG_KEY_START, \
}
#define RG_GAMEPAD_HID_BACK_KEY RG_KEY_SELECT
#define RG_GAMEPAD_HID_LTRIGGER_KEY RG_KEY_MENU
#define RG_GAMEPAD_HID_RTRIGGER_KEY RG_KEY_OPTION

#define RG_MINI_JOYSTICK            1
#define RG_GPIO_I2C_SDA             GPIO_NUM_21
#define RG_GPIO_I2C_SCL             GPIO_NUM_22
#define RG_I2C_SPEED                100000

// The BOOT button is available as the only on-board gamepad input.
#define RG_TOUCH_CS                 GPIO_NUM_33
#define RG_TOUCH_IRQ                GPIO_NUM_36

#define RG_GAMEPAD_GPIO_MAP {\
    {RG_KEY_MENU, .num = GPIO_NUM_0, .pullup = 1, .level = 0},\
}

// The PSRAM modification removes LED1, which otherwise occupies GPIO16 and GPIO17.
// Do not define RG_GPIO_LED for this target.

// SPI display (ST7796)
#define RG_GPIO_LCD_MISO            GPIO_NUM_12
#define RG_GPIO_LCD_MOSI            GPIO_NUM_13
#define RG_GPIO_LCD_CLK             GPIO_NUM_14
#define RG_GPIO_LCD_CS              GPIO_NUM_15
#define RG_GPIO_LCD_DC              GPIO_NUM_2
#define RG_GPIO_LCD_BCKL            GPIO_NUM_27

// SPI microSD card
#define RG_GPIO_SDSPI_MISO          GPIO_NUM_19
#define RG_GPIO_SDSPI_MOSI          GPIO_NUM_23
#define RG_GPIO_SDSPI_CLK           GPIO_NUM_18
#define RG_GPIO_SDSPI_CS            GPIO_NUM_5
