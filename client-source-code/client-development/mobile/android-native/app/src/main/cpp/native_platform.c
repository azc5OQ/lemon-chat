#include "platform/system.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <errno.h>
#include <time.h>
#endif

/**
 * @brief waits while another native caller finishes identity generation
 *
 * @param int64 milliseconds -> wait duration
 *
 * @return void
 */
void desktop_system__sleep(int64 milliseconds)
{
    if (milliseconds <= 0)
    {
        return;
    }
#ifdef _WIN32
    Sleep((DWORD)milliseconds);
#else
    struct timespec delay = { milliseconds / 1000, (milliseconds % 1000) * 1000000 };
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR)
    {
    }
#endif
}
