#ifndef FREERTOS_FREERTOS_H
#define FREERTOS_FREERTOS_H
#include <stddef.h>
typedef int BaseType_t;
typedef unsigned UBaseType_t;
typedef unsigned TickType_t;
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define portMAX_DELAY ((TickType_t)~0U)
#endif
