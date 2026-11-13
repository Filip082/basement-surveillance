#pragma once

/*
 * Pin map for the DFRobot DFR1154 "ESP32-S3 AI CAM" (OV3660).
 *
 * Camera pins match the DFRobot CameraWebServer example and have been verified
 * against the board schematic (also confirmed there: LED GPIO3, IR_CT GPIO47,
 * LTR-308 ambient light sensor on the camera's SCCB bus GPIO8/GPIO9).
 *
 * GPIO 35/36/37 are used internally by the octal PSRAM (ESP32-S3R8) and must
 * never be touched.
 */

/* Camera (DVP) */
#define CAM_PIN_PWDN    (-1)
#define CAM_PIN_RESET   (-1)
#define CAM_PIN_XCLK    5
#define CAM_PIN_SIOD    8   /* SCCB SDA */
#define CAM_PIN_SIOC    9   /* SCCB SCL */

#define CAM_PIN_D0      16  /* Y2 */
#define CAM_PIN_D1      18  /* Y3 */
#define CAM_PIN_D2      21  /* Y4 */
#define CAM_PIN_D3      17  /* Y5 */
#define CAM_PIN_D4      14  /* Y6 */
#define CAM_PIN_D5      7   /* Y7 */
#define CAM_PIN_D6      6   /* Y8 */
#define CAM_PIN_D7      4   /* Y9 */

#define CAM_PIN_VSYNC   1
#define CAM_PIN_HREF    2
#define CAM_PIN_PCLK    15

/* Other on-board peripherals */
#define BOARD_PIN_LED   3   /* on-board LED (GPIO3 is a strapping pin, fine as output after boot) */
#define BOARD_PIN_IR    47  /* IR illumination LEDs */
#define BOARD_PIN_BOOT  0

/* LTR-308ALS-01 ambient light sensor: I2C address 0x53 on the SCCB bus above.
 * Its open-drain interrupt output is wired to GPIO48 (not used by this demo). */
#define BOARD_PIN_LTR_INT 48

/* Also on the schematic (not used by this demo): microSD in SPI mode
 * CS GPIO10, MOSI GPIO11, SCLK GPIO12, MISO GPIO13; native USB GPIO19/GPIO20. */
