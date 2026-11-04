// Target definition
// LCDWIKI ES3C28P: 2.8" 320x240 IPS (ILI9341V), ESP32-S3 16MB flash / 8MB OPI PSRAM, FT6336G touch
#define RG_TARGET_NAME             "ES3C28P"

// Storage (MicroSD wired for SDIO, used in 1-bit mode)
#define RG_STORAGE_ROOT             "/sd"
#define RG_STORAGE_SDMMC_HOST       SDMMC_HOST_SLOT_1
#define RG_STORAGE_SDMMC_SPEED      SDMMC_FREQ_DEFAULT

// Audio (ES8311 codec on I2S, controlled over I2C at 0x18, speaker amp enable is active low)
#define RG_AUDIO_USE_INT_DAC        0
#define RG_AUDIO_USE_EXT_DAC        1
#define RG_AUDIO_CODEC_ES8311       0x18
#define RG_GPIO_SND_I2S_MCLK        GPIO_NUM_4
#define RG_GPIO_SND_I2S_BCK         GPIO_NUM_5
#define RG_GPIO_SND_I2S_WS          GPIO_NUM_7
#define RG_GPIO_SND_I2S_DATA        GPIO_NUM_8
#define RG_GPIO_SND_AMP_ENABLE      GPIO_NUM_1
#define RG_GPIO_SND_AMP_ENABLE_INVERT

// Video
#define RG_SCREEN_DRIVER            0   // 0 = ILI9341/ST7789
#define RG_SCREEN_HOST              SPI2_HOST
#define RG_SCREEN_SPEED             SPI_MASTER_FREQ_40M
#define RG_SCREEN_BACKLIGHT         1
#define RG_SCREEN_WIDTH             320
#define RG_SCREEN_HEIGHT            240
#define RG_SCREEN_ROTATE            0
#define RG_SCREEN_VISIBLE_AREA      {0, 0, 0, 0}
#define RG_SCREEN_SAFE_AREA         {0, 0, 0, 0}
#define RG_SCREEN_INIT()                                                                                         \
    ILI9341_CMD(0xCF, 0x00, 0xc3, 0x30);                                                                         \
    ILI9341_CMD(0xED, 0x64, 0x03, 0x12, 0x81);                                                                   \
    ILI9341_CMD(0xE8, 0x85, 0x00, 0x78);                                                                         \
    ILI9341_CMD(0xCB, 0x39, 0x2c, 0x00, 0x34, 0x02);                                                             \
    ILI9341_CMD(0xF7, 0x20);                                                                                     \
    ILI9341_CMD(0xEA, 0x00, 0x00);                                                                               \
    ILI9341_CMD(0xC0, 0x1B);                 /* Power control   //VRH[5:0] */                                    \
    ILI9341_CMD(0xC1, 0x12);                 /* Power control   //SAP[2:0];BT[3:0] */                            \
    ILI9341_CMD(0xC5, 0x32, 0x3C);           /* VCM control */                                                   \
    ILI9341_CMD(0xC7, 0x91);                 /* VCM control2 */                                                  \
    ILI9341_CMD(0x36, 0x68);                 /* Memory Access Control  (MX|MV|BGR) */                            \
    ILI9341_CMD(0xB1, 0x00, 0x10);           /* Frame Rate Control (1B=70, 1F=61, 10=119) */                     \
    ILI9341_CMD(0xB6, 0x0A, 0xA2);           /* Display Function Control */                                      \
    ILI9341_CMD(0xF6, 0x01, 0x30);                                                                               \
    ILI9341_CMD(0xF2, 0x00);                 /* 3Gamma Function Disable */                                       \
    ILI9341_CMD(0x26, 0x01);                 /* Gamma curve selected */                                          \
    ILI9341_CMD(0xE0, 0x0F, 0x31, 0x2B, 0x0C, 0x0E, 0x08, 0x4E, 0xF1, 0x37, 0x07, 0x10, 0x03, 0x0E, 0x09, 0x00); \
    ILI9341_CMD(0xE1, 0x00, 0x0E, 0x14, 0x03, 0x11, 0x07, 0x31, 0xC1, 0x48, 0x08, 0x0F, 0x0C, 0x31, 0x36, 0x0F); \
    ILI9341_CMD(0x21);                       /* Display inversion on (required by this panel) */

// Input: FT6336 touch zones + BOOT button. Panel is natively 240x320 (portrait); the screen runs in
// landscape so touch coordinates are rotated. Adjust RG_TOUCH_TRANSFORM if the axes feel wrong.
#define RG_TOUCH_TRANSFORM(rx, ry, sx, sy) do { (sx) = (ry); (sy) = 239 - (rx); } while (0)
#define RG_GAMEPAD_TOUCH_MAP {\
    {RG_KEY_UP,     0,   0,  99,  79},\
    {RG_KEY_LEFT,   0,  80,  49, 159},\
    {RG_KEY_RIGHT, 50,  80,  99, 159},\
    {RG_KEY_DOWN,   0, 160,  99, 239},\
    {RG_KEY_A,    260,  40, 319, 119},\
    {RG_KEY_B,    200, 120, 259, 199},\
    {RG_KEY_SELECT, 110, 200, 159, 239},\
    {RG_KEY_START,  160, 200, 209, 239},\
    {RG_KEY_MENU,   110,   0, 209,  39},\
    {RG_KEY_OPTION, 210,   0, 319,  39},\
}
#define RG_GAMEPAD_GPIO_MAP {\
    {RG_KEY_MENU, .num = GPIO_NUM_0, .pullup = 1, .level = 0},\
}

// Battery (divider assumed 1:2 on GPIO9)
#define RG_BATTERY_DRIVER           1
#define RG_BATTERY_ADC_UNIT         ADC_UNIT_1
#define RG_BATTERY_ADC_CHANNEL      ADC_CHANNEL_8
#define RG_BATTERY_CALC_PERCENT(raw) (((raw) * 2.f - 3500.f) / (4200.f - 3500.f) * 100.f)
#define RG_BATTERY_CALC_VOLTAGE(raw) ((raw) * 2.f * 0.001f)

// Status LED: WS2812 on GPIO42, not supported as a plain LED

// I2C bus (shared by touch panel and ES8311 codec)
#define RG_GPIO_I2C_SDA             GPIO_NUM_16
#define RG_GPIO_I2C_SCL             GPIO_NUM_15
#define RG_GPIO_TOUCH_RST           GPIO_NUM_18

// SPI Display (reset is tied to EN)
#define RG_GPIO_LCD_MISO            GPIO_NUM_13
#define RG_GPIO_LCD_MOSI            GPIO_NUM_11
#define RG_GPIO_LCD_CLK             GPIO_NUM_12
#define RG_GPIO_LCD_CS              GPIO_NUM_10
#define RG_GPIO_LCD_DC              GPIO_NUM_46
#define RG_GPIO_LCD_BCKL            GPIO_NUM_45
#define RG_GPIO_LCD_RST             GPIO_NUM_NC

// SDMMC pins
#define RG_GPIO_SDSPI_CLK           GPIO_NUM_38
#define RG_GPIO_SDSPI_CMD           GPIO_NUM_40
#define RG_GPIO_SDSPI_D0            GPIO_NUM_39

// I2S pins for the ES8311 (reserved for future codec support)
// BCK 5, WS 7, DATA_OUT 8, MCLK 4, amp enable GPIO1 (active low)
