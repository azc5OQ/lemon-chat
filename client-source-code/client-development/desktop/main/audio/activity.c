#include "platform/memory.h"
#include "audio/activity.h"
#include "audio/audio.h"
#include "fvad.h"
#include <stdlib.h>

struct desktop_activity_t
{
    Fvad *detector;
    int64 remaining;
    int64 level;
};

/**
 * @brief creates a worker-owned speech detector for 20 ms, 48 kHz microphone frames.
 * @return desktop_activity_t* -> owned detector, or NULL on failure
 */
desktop_activity_t *desktop_activity__create(void)
{
    desktop_activity_t *activity = calloc(1, sizeof(*activity));
    if (!activity)
    {
        return NULL;
    }
    activity->detector = fvad_new();
    if (!activity->detector)
    {
        desktop_memory__erase(activity, sizeof(*activity));
        free(activity);
        return NULL;
    }
    desktop_activity__reset(activity);
    return activity;
}

/**
 * @brief releases the detector after its owning audio worker stops.
 * @param desktop_activity_t* activity -> owned detector, or NULL
 * @return void
 */
void desktop_activity__destroy(desktop_activity_t *activity)
{
    if (activity)
    {
        fvad_free(activity->detector);
        clib__null_memory(activity, sizeof(*activity));
        desktop_memory__erase(activity, sizeof(*activity));
        free(activity);
    }
}

/**
 * @brief resets learned noise and speech history after capture stops or changes mode.
 * @param desktop_activity_t* activity -> worker-owned detector
 * @return void
 */
void desktop_activity__reset(desktop_activity_t *activity)
{
    fvad_reset(activity->detector);
    fvad_set_sample_rate(activity->detector, DESKTOP_AUDIO_RATE);
    fvad_set_mode(activity->detector, 2);
    activity->remaining = 0;
    activity->level = -90;
}

/**
 * @brief combines WebRTC speech classification with a minimum level and a 200 ms trailing hold.
 * @param desktop_activity_t* activity -> worker-owned detector
 * @param const float* mono -> exactly DESKTOP_AUDIO_FRAME normalized samples
 * @param int64 threshold -> minimum speech level in dBFS, -60 through -10
 * @return boole -> TRUE for detected speech or its trailing hold
 */
boole desktop_activity__process(desktop_activity_t *activity, const float *mono, int64 threshold)
{
    short samples[DESKTOP_AUDIO_FRAME] = { 0 };
    double energy = 0;
    float value = 0;
    int64 index = 0;
    int detected = 0;
    threshold = threshold >= -60 && threshold <= -10 ? threshold : -45;
    for (index = 0; index < DESKTOP_AUDIO_FRAME; index++)
    {
        value = mono[index];
        value = value >= -1.0f && value <= 1.0f ? value : 0;
        samples[index] = (short)(value * 32767.0f);
        energy += (double)value * value;
    }
    energy /= DESKTOP_AUDIO_FRAME;
    activity->level = energy > 0.000000001 ? (int64)(4.342944819 * clib__log(energy)) : -90;
    detected = fvad_process(activity->detector, samples, DESKTOP_AUDIO_FRAME);
    clib__null_memory(samples, sizeof(samples));
    if (detected == 1 && activity->level >= threshold)
    {
        activity->remaining = 10;
        return TRUE;
    }
    if (activity->remaining > 0)
    {
        activity->remaining--;
        return TRUE;
    }
    return FALSE;
}

/**
 * @brief returns the most recently processed microphone level.
 * @param const desktop_activity_t* activity -> worker-owned detector
 * @return int64 -> RMS level in dBFS, -90 through zero
 */
int64 desktop_activity__level(const desktop_activity_t *activity)
{
    return activity->level;
}
