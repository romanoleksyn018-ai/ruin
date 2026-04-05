// TFT_eSPI User_Setup.h for RomaOS — ILI9488 480x320
#define ILI9488_DRIVER
#define TFT_WIDTH   320
#define TFT_HEIGHT  480
#define TFT_MOSI  23
#define TFT_SCLK  18
#define TFT_CS    15
#define TFT_DC     2
#define TFT_RST    4
#define SPI_FREQUENCY       27000000
#define SPI_READ_FREQUENCY   4000000
#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define LOAD_GFXFF
#define SMOOTH_FONT
#define ESP32_DMA
