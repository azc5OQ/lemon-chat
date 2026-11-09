#include "platform/memory.h"
// The desktop UI remains usable where game injection has not been implemented.
#include <stdlib.h>
#include "overlay_controller.h"
#include "clib.h"

struct desktop_overlay_controller_t
{
    int64 reserved;
};

/**
 * @brief creates the portable controller without starting any game watcher.
 * @return desktop_overlay_controller_t* -> owned controller, or NULL on allocation failure
 */
desktop_overlay_controller_t *desktop_overlay_controller__create(void)
{
    return calloc(1, sizeof(desktop_overlay_controller_t));
}

/**
 * @brief releases the portable controller.
 * @param desktop_overlay_controller_t* controller -> owned controller
 * @return void
 */
void desktop_overlay_controller__destroy(desktop_overlay_controller_t *controller)
{
    desktop_memory__erase(controller, sizeof(*controller));
    free(controller);
}

/**
 * @brief accepts saved preferences without attempting unsupported game injection.
 * @param desktop_overlay_controller_t* controller -> portable controller
 * @param const desktop_settings_t* settings -> saved preferences
 * @return void
 */
void desktop_overlay_controller__configure(desktop_overlay_controller_t *controller, const desktop_settings_t *settings)
{
    (void)controller;
    (void)settings;
}

/**
 * @brief leaves sample display data in the native view on unsupported overlay platforms.
 * @param desktop_overlay_controller_t* controller -> portable controller
 * @param const desktop_overlay_snapshot_t* snapshot -> display snapshot
 * @return void
 */
void desktop_overlay_controller__publish(desktop_overlay_controller_t *controller, const desktop_overlay_snapshot_t *snapshot)
{
    (void)controller;
    (void)snapshot;
}

/**
 * @brief reports the missing game adapter explicitly instead of claiming an attachment.
 * @param desktop_overlay_controller_t* controller -> portable controller
 * @param char* text -> destination buffer
 * @param uint64 capacity -> destination capacity including the terminator
 * @return void
 */
void desktop_overlay_controller__status(desktop_overlay_controller_t *controller, char *text, uint64 capacity)
{
    (void)controller;
    clib__string_copy(text, capacity, "Game overlay unavailable in this build. Native preview is available.");
}
