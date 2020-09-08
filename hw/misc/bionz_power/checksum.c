#include "qemu/osdep.h"
#include "checksum.h"

uint8_t parity(const uint8_t *data, unsigned int len, unsigned int inc)
{
    int i;
    uint8_t parity = 0x0f;
    assert(inc > 0);
    for (i = 0; i < len; i += inc) {
        parity ^= data[i];
    }
    return parity;
}
