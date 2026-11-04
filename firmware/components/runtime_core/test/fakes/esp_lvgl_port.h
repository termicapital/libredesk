#ifndef ESP_LVGL_PORT_H
#define ESP_LVGL_PORT_H
#include <stdbool.h>
bool lvgl_port_lock(int timeout_ms);
void lvgl_port_unlock(void);
#endif
