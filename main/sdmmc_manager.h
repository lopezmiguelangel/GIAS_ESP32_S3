#ifndef SDMMC_MANAGER_H
#define SDMMC_MANAGER_H

#include <stdbool.h>
#include <time.h>

#define HOURS_IN_DAY 24
#define DAYS_IN_WEEK 7

bool sd_init(bool startup);
void sd_deinit(void);
void sd_test(void);
void sd_read_config(void);
void sd_read_calendar(void);
bool is_sd_mounted(void);
bool sd_get_config(char *ssid, char *password, int *gmt);
void sd_check_calendar(struct tm *current_time, int *estado, int *tiempo_minutos);
bool sd_validate_files(void);
bool sd_get_config(char *ssid, char *password, int *gmt);
bool sd_get_calendar(struct tm *t, int *estado, int *min_out);
bool sd_check_and_create_files(void);
#endif