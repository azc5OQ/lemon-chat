#ifndef MEEK_FONT_FREETYPE_H
#define MEEK_FONT_FREETYPE_H
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_TRUETYPE_TABLES_H
#include FT_OUTLINE_H

// Atlas data contains only measured pixels; no borrowed offsets into font bytes.
typedef struct font_packedchar
{
    unsigned short x0, y0, x1, y1;
    float xoff, yoff, xadvance, xoff2, yoff2;
} font_packedchar;
typedef struct font_quad
{
    float x0, y0, s0, t0, x1, y1, s1, t1;
} font_quad;

static int font_source_open(FT_Library library, const unsigned char* bytes, long size, FT_Face* face)
{
    *face = NULL;
    if (!bytes || size < 12 || size > 32 * 1024 * 1024
        || FT_New_Memory_Face(library, bytes, size, 0, face) != 0)
    {
        return 0;
    }
    if (!FT_IS_SCALABLE(*face) || (*face)->units_per_EM < 16 || (*face)->num_glyphs <= 0
        || (*face)->num_glyphs > 65536 || FT_Select_Charmap(*face, FT_ENCODING_UNICODE) != 0)
    {
        FT_Done_Face(*face);
        *face = NULL;
        return 0;
    }
    return 1;
}

static int font_source_pack(FT_Face face, float size, int oversample_x, int oversample_y,
    unsigned char* pixels, int width, int height, const int* codepoints, int count, font_packedchar* packed)
{
    int x = 1;
    int y = 1;
    int row_height = 0;
    if (!(size > 0 && size <= 512) || width <= 0 || height <= 0 || oversample_x < 1 || oversample_y < 1
        || FT_Set_Char_Size(face, 0, (FT_F26Dot6)(size * 64), 72 * oversample_x, 72 * oversample_y) != 0)
    {
        return 0;
    }
    for (int i = 0; i < count; i++)
    {
        FT_GlyphSlot glyph = face->glyph;
        if (FT_Load_Char(face, codepoints[i], FT_LOAD_NO_HINTING | FT_LOAD_NO_AUTOHINT | FT_LOAD_NO_BITMAP | FT_LOAD_NO_SVG) != 0
            || glyph->format != FT_GLYPH_FORMAT_OUTLINE || FT_Outline_Check(&glyph->outline) != 0)
        {
            return 0;
        }
        FT_BBox bounds;
        FT_Outline_Get_CBox(&glyph->outline, &bounds);
        if (bounds.xMax < bounds.xMin || bounds.yMax < bounds.yMin
            || bounds.xMax - bounds.xMin > (FT_Pos)(width - 2) * 64
            || bounds.yMax - bounds.yMin > (FT_Pos)(height - 2) * 64
            || FT_Render_Glyph(glyph, FT_RENDER_MODE_NORMAL) != 0)
        {
            return 0;
        }
        FT_Bitmap* bitmap = &glyph->bitmap;
        if (bitmap->pixel_mode != FT_PIXEL_MODE_GRAY || bitmap->width > (unsigned int)(width - 2)
            || bitmap->rows > (unsigned int)(height - 2))
        {
            return 0;
        }
        if (x + (int)bitmap->width + 1 > width)
        {
            x = 1;
            y += row_height + 1;
            row_height = 0;
        }
        if (y + (int)bitmap->rows + 1 > height)
        {
            return 0;
        }
        for (unsigned int row = 0; row < bitmap->rows; row++)
        {
            const unsigned char* source = bitmap->pitch >= 0 ? bitmap->buffer + row * bitmap->pitch
                : bitmap->buffer + (bitmap->rows - 1 - row) * (size_t)(-bitmap->pitch);
            for (unsigned int column = 0; column < bitmap->width; column++)
            {
                pixels[(y + row) * width + x + column] = source[column];
            }
        }
        packed[i].x0 = (unsigned short)x;
        packed[i].y0 = (unsigned short)y;
        packed[i].x1 = (unsigned short)(x + bitmap->width);
        packed[i].y1 = (unsigned short)(y + bitmap->rows);
        packed[i].xoff = (float)glyph->bitmap_left / oversample_x;
        packed[i].yoff = -(float)glyph->bitmap_top / oversample_y;
        packed[i].xoff2 = packed[i].xoff + (float)bitmap->width / oversample_x;
        packed[i].yoff2 = packed[i].yoff + (float)bitmap->rows / oversample_y;
        packed[i].xadvance = (float)glyph->advance.x / (64 * oversample_x);
        x += bitmap->width + 1;
        if ((int)bitmap->rows > row_height)
        {
            row_height = bitmap->rows;
        }
    }
    return 1;
}
#endif
