#ifndef POWER_H
#define POWER_H

void power_init(void);

void power_sd_rtc_on(void);
void power_sd_rtc_off(void);
void power_sd_rtc_hold(void);

void power_i2s_on(void);
void power_i2s_off(void);
void power_i2s_hold(void);
void power_deep_sleep(int minutos);

#endif