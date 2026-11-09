#ifndef LEMON_DESKTOP_TREE_ICONS_H
#define LEMON_DESKTOP_TREE_ICONS_H

#include "clib.h"

boole desktop_tree_icons__decode(const char *data, ubyte *pixels, uint64 capacity);
boole desktop_tree_icons__flag(const char *country, ubyte *pixels, uint64 capacity);

#endif
