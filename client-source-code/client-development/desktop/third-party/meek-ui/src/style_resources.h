#ifndef MEEK_STYLE_RESOURCES_H
#define MEEK_STYLE_RESOURCES_H

#include "types.h"

// Internal stylesheet resource ownership. All calls run on the drawing thread.
boole style_resources__prepare(const char* source, int64 length, const char* label, char** output, int64* output_length);
char* style_resources__read(const char* key, int64* size);
void style_resources__clear(void);

#endif
