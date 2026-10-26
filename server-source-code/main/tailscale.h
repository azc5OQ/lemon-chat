#ifndef TAILSCALE_H
#define TAILSCALE_H

#include "definitions.h"

boole tailscale__configure(void);
boole tailscale__is_available(void);
boole tailscale__is_ready(void);
void tailscale__print_addresses(void);

#endif
