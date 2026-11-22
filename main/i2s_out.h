#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t i2s_out_init(void);
esp_err_t i2s_out_apply_fs(uint32_t fs);
esp_err_t i2s_out_write(const void *pcm, size_t bytes);
esp_err_t i2s_out_deinit(void);
uint32_t  i2s_out_current_fs(void);
void      i2s_out_set_paused(bool paused);
bool      i2s_out_paused(void);
