#include "als.h"

#include <math.h>
#include "esp_log.h"
#include "driver/i2c_master.h"

static const char *TAG = "als";

#define LTR308_ADDR        0x53
#define REG_MAIN_CTRL      0x00
#define REG_ALS_MEAS_RATE  0x04
#define REG_ALS_GAIN       0x05
#define REG_PART_ID        0x06
#define REG_ALS_DATA_0     0x0D   /* 20-bit result in 0x0D..0x0F */

#define ALS_GAIN_X         3.0f   /* register value 1 = 3x */
#define ALS_INT_FACTOR     1.0f   /* 18-bit / 100 ms */

#define I2C_TIMEOUT_MS     50

static i2c_master_dev_handle_t s_dev;

static esp_err_t reg_read(uint8_t reg, uint8_t *buf, size_t n)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, n, I2C_TIMEOUT_MS);
}

static esp_err_t reg_write(uint8_t reg, uint8_t val)
{
    const uint8_t b[2] = { reg, val };
    return i2c_master_transmit(s_dev, b, sizeof(b), I2C_TIMEOUT_MS);
}

bool als_available(void)
{
    return s_dev != NULL;
}

esp_err_t als_init(void)
{
    /* The camera driver already owns the bus; find it (the port it picked varies). */
    i2c_master_bus_handle_t bus = NULL;
    for (int port = 1; port >= 0 && !bus; port--) {
        i2c_master_bus_handle_t h = NULL;
        if (i2c_master_get_bus_handle(port, &h) == ESP_OK && h) {
            bus = h;
            ESP_LOGI(TAG, "using the camera's I2C bus (port %d)", port);
        }
    }
    if (!bus) {
        ESP_LOGW(TAG, "camera I2C bus not found - ambient light sensor unavailable");
        return ESP_ERR_NOT_FOUND;
    }

    /* Scan so the log shows what is really on the bus (camera = 0x3c). */
    char list[128] = "";
    size_t pos = 0;
    for (int addr = 0x08; addr < 0x78; addr++) {
        if (i2c_master_probe(bus, addr, 20) == ESP_OK && pos + 6 < sizeof(list)) {
            pos += snprintf(list + pos, sizeof(list) - pos, "0x%02x ", addr);
        }
    }
    ESP_LOGI(TAG, "I2C devices on bus: %s", pos ? list : "(none)");

    const i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = LTR308_ADDR,
        .scl_speed_hz = 100000,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &dc, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "add device failed: %s", esp_err_to_name(err));
        s_dev = NULL;
        return err;
    }

    uint8_t id = 0;
    err = reg_read(REG_PART_ID, &id, 1);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "LTR-308 (0x53) did not answer: %s - using camera exposure instead",
                 esp_err_to_name(err));
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
        return err;
    }
    ESP_LOGI(TAG, "LTR-308 part id 0x%02x (expected 0xb1)", id);

    err = reg_write(REG_ALS_MEAS_RATE, 0x22);          /* 18-bit, 100 ms */
    if (err == ESP_OK) err = reg_write(REG_ALS_GAIN, 0x01);   /* gain 3x */
    if (err == ESP_OK) err = reg_write(REG_MAIN_CTRL, 0x02);  /* ALS active */
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "LTR-308 configuration failed: %s", esp_err_to_name(err));
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
        return err;
    }
    ESP_LOGI(TAG, "LTR-308 ambient light sensor ready");
    return ESP_OK;
}

esp_err_t als_read_lux(float *lux)
{
    if (!s_dev) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t d[3];
    esp_err_t err = reg_read(REG_ALS_DATA_0, d, sizeof(d));
    if (err != ESP_OK) {
        return err;
    }
    uint32_t raw = (uint32_t)d[0] | ((uint32_t)d[1] << 8) | ((uint32_t)(d[2] & 0x0f) << 16);
    *lux = 0.6f * (float)raw / (ALS_GAIN_X * ALS_INT_FACTOR);
    return ESP_OK;
}

int als_lux_to_level(float lux)
{
    if (lux < 0) {
        lux = 0;
    }
    float level = log10f(lux + 1.0f) / 4.7f * 100.0f;      /* 10^4.7 ~ 50 000 lux */
    if (level > 100) level = 100;
    return (int)(level + 0.5f);
}
