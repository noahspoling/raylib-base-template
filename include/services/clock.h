#ifndef CLOCK_H
#define CLOCK_H

#include <stdbool.h>

typedef struct {
    float realtimeDelta;
    bool  turnPending;
    int   tickToSimulate; // units are turns, could be actions take multiple ticks or just a turn system;
} Clock;

void frame_tick(Clock *clock);



#endif // CLOCK_H