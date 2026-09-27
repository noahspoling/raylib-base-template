#ifndef CLOCK_H
#define CLOCK_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    float   realtimeDelta;
    bool    turnPending;
    int32_t tickToSimulate;
} Clock;

void frame_tick(Clock *clock);



#endif