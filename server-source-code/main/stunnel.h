#ifndef STUNNEL_H
#define STUNNEL_H

#include "definitions.h"

void stunnel__launch(void);
void stunnel__renew_certificate_if_due(void);
boole stunnel__was_started(void);

#endif
