#ifndef TASK7_FAKE_TASK_H
#define TASK7_FAKE_TASK_H

#include "FreeRTOS.h"

TaskHandle_t xTaskCreateStatic(TaskFunction_t task_code,
                               const char *name,
                               uint32_t stack_depth,
                               void *argument,
                               UBaseType_t priority,
                               StackType_t *stack_buffer,
                               StaticTask_t *task_buffer);
void vTaskDelay(TickType_t ticks);

#endif
