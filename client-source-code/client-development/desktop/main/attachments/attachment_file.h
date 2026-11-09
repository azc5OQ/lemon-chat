#ifndef LEMON_DESKTOP_ATTACHMENT_FILE_H
#define LEMON_DESKTOP_ATTACHMENT_FILE_H

#include "clib.h"

ubyte *desktop_attachment_file__read(const char *path, uint64 limit, uint64 *size);
boole desktop_attachment_file__save(const char *path, const ubyte *bytes, uint64 size);
void desktop_attachment_file__name(char *name, uint64 capacity, const char *source);
boole desktop_attachment_file__home(char *path, uint64 capacity);

#endif
