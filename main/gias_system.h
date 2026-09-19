#ifndef GIAS_SYSTEM_H
#define GIAS_SYSTEM_H

#include <time.h>

typedef struct {
    int estado;
    int minutos;
    struct tm hora;
} gias_status_t;

gias_status_t gias_get_status(void);
void gias_record_start(int minutos, struct tm *current_time);

#endif