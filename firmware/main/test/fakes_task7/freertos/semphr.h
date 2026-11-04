#ifndef TASK7_FAKE_SEMPHR_H
#define TASK7_FAKE_SEMPHR_H
#include "freertos/FreeRTOS.h"
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *buffer);
BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t timeout);
BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore);
#endif
