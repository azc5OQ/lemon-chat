#ifndef LOGIN_ADMISSION_H
#define LOGIN_ADMISSION_H

#include "../definitions.h"

void login_admission__set_limit(uint64 limit);
boole login_admission__acquire(boole (*connection_is_valid)(void *), void *connection, uint64 timeout_ms);
void login_admission__release(void);

#endif
