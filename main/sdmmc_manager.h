#ifndef SDMMC_MANAGER_H
#define SDMMC_MANAGER_H

#include <stdbool.h>
#include <time.h>

bool sd_init(bool startup);
void sd_deinit(void);
bool is_sd_mounted(void);
bool sd_get_config(char *ssid, char *password, int *gmt);
void sd_check_calendar(struct tm *current_time, int *estado, int *tiempo_minutos);
bool sd_validate_files(void);
bool sd_check_and_create_files(void);
void sd_power_on(void);
void sd_power_off(void);

#endif