#ifndef GIAS_SYSTEM_H
#define GIAS_SYSTEM_H

#include <time.h>

typedef struct {
    int estado;
    int minutos;
    struct tm hora;
} gias_status_t;

void gias_init(void);
gias_status_t gias_get_status(void);
void gias_record_start(int minutos, struct tm *current_time);
void gias_deep_sleep(int minutos);
void gias_led_red(void);
void gias_led_green(void);
void gias_led_off(void);
void gias_log_init(void);
void gias_log_flush(void);
void gias_error_handler(int titileos);

#endif