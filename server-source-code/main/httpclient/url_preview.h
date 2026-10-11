#ifndef URL_PREVIEW_H
#define URL_PREVIEW_H



typedef struct url_preview_t
{
    char url[2049];
    char title[513];
    char description[1025];
    char site_name[129];
    char image_url[2049];
    char* image_data;
} url_preview_t;

int url_preview__resolve(const char* url, url_preview_t* preview);
void url_preview__free(url_preview_t* preview);
void url_preview__read_html(const char* html, url_preview_t* preview);
int url_preview__absolute_url(const char* base, const char* reference, char* output, unsigned int capacity);
#endif
