#ifndef LOGIN_ADMISSION_H
#define LOGIN_ADMISSION_H

#include "definitions.h"

void login_admission__set_limit(uint limit);
int login_admission__acquire(int (*connection_is_valid)(void*), void* connection, uint timeout_ms);
void login_admission__release(void);

#endif
