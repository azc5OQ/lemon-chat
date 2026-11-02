// backends/embedded/embedded_win32_input.c
//
// Translates the host window's messages into meek-ui input. The host
// keeps its own window procedure (or hooks the target's) and forwards
// every message here first; a TRUE return means the UI consumed it and
// the host should not act on it. Left-button sequences stay with whichever
// side owned their press; crossing a panel cannot steal the host's release.
// Idle hover/wheel use hit testing; keyboard input uses the char-redirect gate.
// The root
// window itself normally covers the whole frame transparently, so a
// hit on it means "over the game", not "over the overlay".

#include "embedded_win32_input.h"

#include "gui.h"
#include "scene.h"

static boole g_embedded_win32_captured = FALSE;
typedef enum embedded_pointer_owner_e
{
    EMBEDDED_POINTER__NONE,
    EMBEDDED_POINTER__HOST,
    EMBEDDED_POINTER__UI
} embedded_pointer_owner_e;
static embedded_pointer_owner_e g_embedded_win32_left_owner = EMBEDDED_POINTER__NONE;
// A cancelled UI gesture must not leak its eventual release into the host,
// which never received the corresponding press. A new down starts a new pair.
static boole g_embedded_win32_cancelled_release = FALSE;

/**
 * @brief tells whether a client-space point is over a meek-ui node other than the root window
 *
 * @param int64 x -> client x in pixels
 * @param int64 y -> client y in pixels
 *
 * @return boole -> TRUE when the overlay owns that point
 */
boole embedded_win32__pointer_over_ui(int64 x, int64 y)
{
    gui_node_t* root = scene__root();
    gui_node_t* hit = NULL_POINTER;

    if (root == NULL_POINTER)
    {
        return FALSE;
    }

    hit = scene__hit_test(root, x, y);

    if (hit == NULL_POINTER || hit == root)
    {
        return FALSE;
    }

    return TRUE;
}

/**
 * @brief forwards one window message to meek-ui and reports whether the UI consumed it
 *
 * @param HWND hwnd -> the host window
 * @param UINT msg -> message id
 * @param WPARAM wparam -> message word parameter
 * @param LPARAM lparam -> message long parameter
 *
 * @attention mouse coordinates are client pixels; wheel messages arrive in screen coordinates and are converted here. Only the left button is forwarded, matching what meek-ui's own platform backends do
 *
 * @return boole -> TRUE when the host should not process the message further
 */
boole embedded_win32__on_window_event(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    int64 x = (int64)(short)LOWORD(lparam);
    int64 y = (int64)(short)HIWORD(lparam);
    POINT p;
    float wheel = 0.0f;
    boole over = FALSE;

    switch (msg)
    {
        case WM_MOUSEMOVE:
            if (g_embedded_win32_left_owner == EMBEDDED_POINTER__HOST ||
                (g_embedded_win32_left_owner == EMBEDDED_POINTER__NONE &&
                 (wparam & (MK_LBUTTON | MK_RBUTTON | MK_MBUTTON | MK_XBUTTON1 | MK_XBUTTON2))))
            {
                return FALSE;
            }
            scene__on_mouse_move(x, y);
            return (g_embedded_win32_left_owner == EMBEDDED_POINTER__UI) ? TRUE : embedded_win32__pointer_over_ui(x, y);

        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
            over = embedded_win32__pointer_over_ui(x, y);
            g_embedded_win32_cancelled_release = FALSE;
            g_embedded_win32_left_owner = over ? EMBEDDED_POINTER__UI : EMBEDDED_POINTER__HOST;

            if (over == TRUE)
            {
                SetCapture(hwnd);
                g_embedded_win32_captured = (GetCapture() == hwnd) ? TRUE : FALSE;
                scene__on_mouse_button(0, TRUE, x, y);
            }
            else
            {
                // A transparent root is layout space, not an input target.
                scene__cancel_press();
            }
            return over;

        case WM_LBUTTONUP:
        {
            boole ui_press = (g_embedded_win32_left_owner == EMBEDDED_POINTER__UI);
            over = (ui_press || g_embedded_win32_cancelled_release) ? TRUE : FALSE;
            g_embedded_win32_left_owner = EMBEDDED_POINTER__NONE;
            g_embedded_win32_cancelled_release = FALSE;

            if (g_embedded_win32_captured == TRUE)
            {
                // ReleaseCapture synchronously sends WM_CAPTURECHANGED. Mark
                // our intentional release first so it does not cancel this click.
                g_embedded_win32_captured = FALSE;
                if (GetCapture() == hwnd)
                {
                    ReleaseCapture();
                }
            }

            if (ui_press)
            {
                scene__on_mouse_button(0, FALSE, x, y);
            }
            return over;
        }

        case WM_CAPTURECHANGED:
            if (g_embedded_win32_captured == TRUE && (HWND)lparam != hwnd)
            {
                g_embedded_win32_captured = FALSE;
                g_embedded_win32_cancelled_release = (g_embedded_win32_left_owner == EMBEDDED_POINTER__UI);
                g_embedded_win32_left_owner = EMBEDDED_POINTER__NONE;
                scene__cancel_press();
            }
            // The host must also observe its capture ownership changing.
            return FALSE;

        case WM_CANCELMODE:
        case WM_KILLFOCUS:
        {
            boole had_capture = g_embedded_win32_captured;
            g_embedded_win32_captured = FALSE;
            if (g_embedded_win32_left_owner == EMBEDDED_POINTER__UI)
            {
                g_embedded_win32_cancelled_release = TRUE;
            }
            g_embedded_win32_left_owner = EMBEDDED_POINTER__NONE;
            scene__cancel_press();
            if (had_capture == TRUE && GetCapture() == hwnd)
            {
                ReleaseCapture();
            }
            return FALSE;
        }

        case WM_MOUSEWHEEL:
            p.x = (LONG)(short)LOWORD(lparam);
            p.y = (LONG)(short)HIWORD(lparam);
            ScreenToClient(hwnd, &p);
            wheel = (float)GET_WHEEL_DELTA_WPARAM(wparam) / (float)WHEEL_DELTA;
            over = embedded_win32__pointer_over_ui((int64)p.x, (int64)p.y);

            if (over == TRUE)
            {
                scene__on_mouse_wheel((int64)p.x, (int64)p.y, wheel);
            }

            return over;

        case WM_CHAR:
            if (scene__has_char_redirect() == TRUE)
            {
                scene__on_char((uint)wparam);
                return TRUE;
            }

            return FALSE;

        case WM_KEYDOWN:
        case WM_KEYUP:
            if (scene__has_char_redirect() == TRUE)
            {
                scene__on_key((int64)wparam, (msg == WM_KEYDOWN) ? TRUE : FALSE);
                return TRUE;
            }

            return FALSE;

        default:
            break;
    }

    return FALSE;
}
