#ifndef FREERTOS_TASK_H
#define FREERTOS_TASK_H
#include "freertos/FreeRTOS.h"
typedef void (*TaskFunction_t)(void *);
BaseType_t xTaskCreate(TaskFunction_t task, const char *name, unsigned stack, void *argument, UBaseType_t priority, void *handle);
void vTaskDelete(void *task);
#endif
