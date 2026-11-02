#ifndef MEEK_UI_EFFECTS_PROFILE_H
#define MEEK_UI_EFFECTS_PROFILE_H

/* Internal diagnostic only. No counters or clock calls in normal builds.
 * Enable with -DMEEK_UI_PROFILE_GPU_CALLS in a separate diagnostic build. */
#ifdef MEEK_UI_PROFILE_GPU_CALLS
#include "gui_api.h"
enum effects_profile_stage
{
    EFFECTS_PROFILE_BACKGROUND_UPLOAD,
    EFFECTS_PROFILE_BACKGROUND_SUBMIT,
    EFFECTS_PROFILE_BACKGROUND_MAP,
    EFFECTS_PROFILE_BACKGROUND_COPY,
    EFFECTS_PROFILE_COMPOSE_UPLOAD,
    EFFECTS_PROFILE_COMPOSE_SUBMIT,
    EFFECTS_PROFILE_COMPOSE_MAP,
    EFFECTS_PROFILE_COMPOSE_COPY,
    EFFECTS_PROFILE_PRESENT,
    // layer mode: the back-buffer read-back (copy, then the blocking map) and the layer texture uploads
    EFFECTS_PROFILE_READBACK_COPY,
    EFFECTS_PROFILE_READBACK_MAP,
    EFFECTS_PROFILE_LAYER_UPLOAD,
    EFFECTS_PROFILE_STAGE_COUNT
};
long long effects_profile__now(void);
void effects_profile__step(enum effects_profile_stage stage, long long* previous);
GUI_API void effects_profile__reset(void);
#define EFFECTS_PROFILE_BEGIN(name) long long name = effects_profile__now()
#define EFFECTS_PROFILE_STEP(name, stage) effects_profile__step(stage, &name)
#else
#define EFFECTS_PROFILE_BEGIN(name) do { } while (0)
#define EFFECTS_PROFILE_STEP(name, stage) do { } while (0)
#endif
#endif
