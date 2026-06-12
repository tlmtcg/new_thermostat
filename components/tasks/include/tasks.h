#ifndef TASKS_H
#define TASKS_H

#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

typedef struct
{
    TaskFunction_t function;
    const char *name;
    uint32_t stack_size;
    void *config;
    UBaseType_t priority;
} task_definition_t;

void tasks_start(void);

#endif
