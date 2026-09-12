// i2s_audio.h
#ifndef I2S_AUDIO_H
#define I2S_AUDIO_H

#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void i2s_init(void);
void i2s_deinit(void);
size_t i2s_read_samples(int16_t *buffer, size_t samples_to_read, TickType_t timeout);

#endif