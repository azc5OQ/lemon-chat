#ifndef IP_TOOLS_H
#define IP_TOOLS_H

#include "definitions.h"

uint ip_tools__get_local_ipv4_addresses(char addresses[][IPV4_ADDRESS_STRING_LENGTH], uint capacity);
void ip_tools_load_iso_country_code(char* ip_address_to_resolve, char* out_memory_to_write_country_code_to);

#endif
