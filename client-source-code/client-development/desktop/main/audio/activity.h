#ifndef LEMON_DESKTOP_ACTIVITY_H
#define LEMON_DESKTOP_ACTIVITY_H

#include "clib.h"

typedef struct desktop_activity_t desktop_activity_t;

desktop_activity_t *desktop_activity__create(void);
void desktop_activity__destroy(desktop_activity_t *activity);
void desktop_activity__reset(desktop_activity_t *activity);
boole desktop_activity__process(desktop_activity_t *activity, const float *mono, int64 threshold);
int64 desktop_activity__level(const desktop_activity_t *activity);

#endif
