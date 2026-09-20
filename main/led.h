#ifndef LED_H
#define LED_H

extern TaskHandle_t led_task_handle;


void led_init(void);
void gias_error_handler(int titileos);
void led_blink_count(int cantidad);
void led_status_task(void *pvParameters);

#endif