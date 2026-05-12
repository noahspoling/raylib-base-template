#ifndef GLOBAL_STATE_H
#define GLOBAL_STATE_H

#include "raylib.h"

typedef struct GlobalState {
    float total_time;
    int frame_count;
    Camera2D camera;
} GlobalState;

#endif
