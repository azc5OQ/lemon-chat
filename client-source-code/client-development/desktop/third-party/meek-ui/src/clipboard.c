#if defined(_WIN32)
#include <windows.h>
#endif

#include "clipboard.h"
#include "clib/stdlib.h"

static gui_clipboard_read_fn clipboard__reader;
static gui_clipboard_write_fn clipboard__writer;

void clipboard__set_callbacks(gui_clipboard_read_fn read, gui_clipboard_write_fn write)
{
    clipboard__reader = read;
    clipboard__writer = write;
}

boole clipboard__read(char* output, int64 capacity)
{
    if (output == NULL_POINTER || capacity < 1)
    {
        return FALSE;
    }
    output[0] = 0;
    if (clipboard__reader != NULL_POINTER)
    {
        return clipboard__reader(output, capacity);
    }
#if defined(_WIN32)
    HGLOBAL data = NULL;
    const wchar_t* wide = NULL;
    SIZE_T count = 0;
    SIZE_T i = 0;
    int64 used = 0;
    boole ok = FALSE;
    if (!OpenClipboard(NULL))
    {
        return FALSE;
    }
    data = GetClipboardData(CF_UNICODETEXT);
    if (data != NULL)
    {
        count = GlobalSize(data) / sizeof(wchar_t);
        wide = GlobalLock(data);
    }
    if (wide != NULL)
    {
        while (i < count && wide[i] != 0)
        {
            char encoded[4];
            int units = 1;
            int bytes = 0;
            if (wide[i] >= 0xd800 && wide[i] <= 0xdbff && i + 1 < count)
            {
                units = 2;
            }
            bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide + i, units, encoded, sizeof(encoded), NULL, NULL);
            if (bytes == 0)
            {
                break;
            }
            if (used + bytes >= capacity)
            {
                ok = TRUE;
                break;
            }
            stdlib__memcpy(output + used, encoded, bytes);
            used += bytes;
            i += units;
        }
        if (i < count && wide[i] == 0)
        {
            ok = TRUE;
        }
        GlobalUnlock(data);
    }
    CloseClipboard();
    output[ok ? used : 0] = 0;
    return ok;
#else
    return FALSE;
#endif
}

boole clipboard__write(const char* text, int64 length)
{
    if (text == NULL_POINTER || length < 0 || length > 0x7ffffffe)
    {
        return FALSE;
    }
    if (clipboard__writer != NULL_POINTER)
    {
        return clipboard__writer(text, length);
    }
#if defined(_WIN32)
    int count = length ? MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, (int)length, NULL, 0) : 0;
    HGLOBAL data = NULL;
    wchar_t* wide = NULL;
    HWND owner = GetActiveWindow();
    HWND temporary = NULL;
    boole ok = FALSE;
    if (length && count == 0)
    {
        return FALSE;
    }
    data = GlobalAlloc(GMEM_MOVEABLE, ((SIZE_T)count + 1) * sizeof(wchar_t));
    if (data == NULL)
    {
        return FALSE;
    }
    wide = GlobalLock(data);
    if (wide == NULL)
    {
        GlobalFree(data);
        return FALSE;
    }
    if (count)
    {
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, (int)length, wide, count);
    }
    wide[count] = 0;
    GlobalUnlock(data);
    // EmptyClipboard needs a real owner. A hidden host may have no active
    // window; use a message-only owner without showing or focusing anything.
    if (owner == NULL)
    {
        temporary = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, GetModuleHandleW(NULL), NULL);
        owner = temporary;
    }
    if (owner != NULL && OpenClipboard(owner))
    {
        if (EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, data) != NULL)
        {
            data = NULL;
            ok = TRUE;
        }
        CloseClipboard();
    }
    if (temporary != NULL)
    {
        DestroyWindow(temporary);
    }
    if (data != NULL)
    {
        GlobalFree(data);
    }
    return ok;
#else
    return FALSE;
#endif
}
