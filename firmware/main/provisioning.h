#ifndef LIBRE_DESK_PROVISIONING_H
#define LIBRE_DESK_PROVISIONING_H

typedef enum provisioning_result {
    PROVISIONING_RESULT_OK = 0,
    PROVISIONING_RESULT_ALREADY_STARTED,
    PROVISIONING_RESULT_IO_ERROR,
    PROVISIONING_RESULT_COUNT
} provisioning_result_t;

provisioning_result_t provisioning_reset_monitor_start(void);

#endif
