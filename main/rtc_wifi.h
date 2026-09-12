#ifndef RTC_WIFI_H
#define RTC_WIFI_H

#include <time.h>
#include "driver/gpio.h"

#define RTC_I2C_SDA_PIN  GPIO_NUM_13
#define RTC_I2C_SCL_PIN  GPIO_NUM_14

struct tm rtc_wifi_sync(const char *ssid, const char *password, int gmt);

#endif