#ifndef TASK7_FAKE_FREERTOS_H
#define TASK7_FAKE_FREERTOS_H

#include <stdint.h>

typedef int BaseType_t;
typedef unsigned int UBaseType_t;
typedef unsigned int TickType_t;
typedef unsigned char StackType_t;
typedef void *TaskHandle_t;
typedef struct { unsigned char bytes[128]; } StaticTask_t;
typedef union { long double alignment; void *pointer; unsigned char bytes[128]; } StaticSemaphore_t;
typedef void (*TaskFunction_t)(void *);
typedef int portMUX_TYPE;
typedef void *SemaphoreHandle_t;

typedef int SemaphoreTakeResult;
#define semaphoreTAKE_OK 1
#define semaphoreTAKE_FAIL 0

#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define pdFAIL 0
#define tskIDLE_PRIORITY 0U
#define portMAX_DELAY ((TickType_t)0xffffffffU)
#define portMUX_INITIALIZER_UNLOCKED 0
#define pdMS_TO_TICKS(milliseconds) ((TickType_t)(milliseconds))

void task7_enter_critical(portMUX_TYPE *mux);
void task7_exit_critical(portMUX_TYPE *mux);
#define taskENTER_CRITICAL(mux) task7_enter_critical((mux))
#define taskEXIT_CRITICAL(mux) task7_exit_critical((mux))

#endif
