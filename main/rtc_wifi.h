#ifndef RTC_WIFI_H
#define RTC_WIFI_H

#include <time.h>
#include "driver/gpio.h"

// Pines I2C del RTC
#define RTC_I2C_SDA_PIN  GPIO_NUM_13
#define RTC_I2C_SCL_PIN  GPIO_NUM_14

// Sincroniza hora (WiFi + SNTP + RTC) y devuelve la hora elegida
struct tm rtc_wifi_sync(const char *ssid, const char *password, int gmt);

// Libera el bus I2C y deja los pines en alta impedancia
void rtc_i2c_deinit(void);

#endif