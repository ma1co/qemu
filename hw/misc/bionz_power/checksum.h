#ifndef QEMU_BIONZ_POWER_CHECKSUM_H
#define QEMU_BIONZ_POWER_CHECKSUM_H

uint8_t parity(const uint8_t *data, unsigned int len, unsigned int inc);

#endif /* QEMU_BIONZ_POWER_CHECKSUM_H */
