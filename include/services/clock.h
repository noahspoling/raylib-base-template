#ifndef CLOCK_H
#define CLOCK_H

#include <stdbool.h>

typedef struct {
    float realtimeDelta;
    bool  turnPending;
    int   tickToSimulate;
} Clock;

void frame_tick(Clock *clock);



#endif