#include "platform/hotkey.h"

// F1..F12 retain their original saved values; printable keys use ASCII.
static const struct
{
    int64 key;
    const char* name;
} g_keys[] =
{
    {'Q', "Q"},
    {'A', "A"}, {'B', "B"}, {'C', "C"}, {'D', "D"}, {'E', "E"}, {'F', "F"},
    {'G', "G"}, {'H', "H"}, {'I', "I"}, {'J', "J"}, {'K', "K"}, {'L', "L"},
    {'M', "M"}, {'N', "N"}, {'O', "O"}, {'P', "P"}, {'R', "R"}, {'S', "S"},
    {'T', "T"}, {'U', "U"}, {'V', "V"}, {'W', "W"}, {'X', "X"}, {'Y', "Y"}, {'Z', "Z"},
    {'0', "0"}, {'1', "1"}, {'2', "2"}, {'3', "3"}, {'4', "4"},
    {'5', "5"}, {'6', "6"}, {'7', "7"}, {'8', "8"}, {'9', "9"},
    {' ', "Space"},
    {1, "F1"}, {2, "F2"}, {3, "F3"}, {4, "F4"}, {5, "F5"}, {6, "F6"},
    {7, "F7"}, {8, "F8"}, {9, "F9"}, {10, "F10"}, {11, "F11"}, {12, "F12"}
};

/**
 * @brief returns the number of supported global push-to-talk keys.
 * @return int64 -> key count
 */
int64 desktop_hotkey__count(void)
{
    return sizeof(g_keys) / sizeof(g_keys[0]);
}

/**
 * @brief resolves a picker position to a persistent, platform-independent key.
 * @param int64 index -> zero-based picker position
 * @return int64 -> key value, or Q for an invalid position
 */
int64 desktop_hotkey__key(int64 index)
{
    return index >= 0 && index < desktop_hotkey__count() ? g_keys[index].key : DESKTOP_HOTKEY_DEFAULT;
}

/**
 * @brief validates a saved key and locates its picker position.
 * @param int64 key -> printable ASCII key or legacy F-key number
 * @return int64 -> picker position, or -1 when unsupported
 */
int64 desktop_hotkey__index(int64 key)
{
    int64 index = 0;
    for (index = 0; index < desktop_hotkey__count(); index++)
    {
        if (g_keys[index].key == key)
        {
            return index;
        }
    }
    return -1;
}

/**
 * @brief obtains the display name shared by settings and live voice status.
 * @param int64 key -> persistent key value
 * @return const char* -> borrowed immutable caption
 */
const char* desktop_hotkey__name(int64 key)
{
    int64 index = desktop_hotkey__index(key);
    return index >= 0 ? g_keys[index].name : "Unknown";
}
