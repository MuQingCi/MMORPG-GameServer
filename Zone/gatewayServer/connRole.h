#ifndef CLEARMOON_GATEWAY_CONNROLE_H
#define CLEARMOON_GATEWAY_CONNROLE_H

#include <cstdint>

enum connRole : uint8_t
{
    Client  = 1,
    Backend = 2
};

#endif