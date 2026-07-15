#ifndef FIRST_TIME_SETUP_H
#define FIRST_TIME_SETUP_H 1

extern const char* g_mark_info;
extern const char* g_mark_ok;
extern const char* g_mark_off;
extern const char* g_mark_warn;
extern const char* g_mark_ask;


extern char g_first_run_admin_password[];

void first_time_setup__run(char plaintext_keys[][256]);

#endif
