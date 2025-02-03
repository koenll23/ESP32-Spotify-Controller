// user setup, might help configuring and setting up your project! :D

#define USER_SETUP_INFO "User_Setup"
#define ST7735_DRIVER  // Selecteer de ST7735 driver

// Pin-configuration of this project, make sure to match this
#define TFT_MOSI 23    // SDA
#define TFT_SCLK 18    // SCK
#define TFT_CS   15    // CS
#define TFT_DC   2     // AO
#define TFT_RST  4     // RESET
// gnd to gnd
// vcc to 3.3v
// LED / backlight to 3.3v

// Displayresolution
#define TFT_WIDTH  128
#define TFT_HEIGHT 160


// colour-order
#define TFT_RGB_ORDER TFT_RGB


// SPI-speed / frequency (27 max)
#define SPI_FREQUENCY  27000000  // 27 MHz

#define ST7735_REDTAB

// activating fonts
#define LOAD_GLCD    // Standaard 6x8 pixels
#define LOAD_FONT2   // Klein (5x7 pixels)
#define LOAD_FONT4   // Middelgroot (6x8 pixels)
#define LOAD_FONT6   // Groot (7x12 pixels)
#define LOAD_FONT7   // Extra groot (8x16 pixels)
#define LOAD_FONT8   // Zeer groot (16x26 pixels)
