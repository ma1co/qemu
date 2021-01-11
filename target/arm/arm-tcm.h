#ifndef QEMU_ARM_TCM_H
#define QEMU_ARM_TCM_H

#include "cpu.h"

typedef struct arm_tcm_mem {
    MemoryRegion *mr[2];
    uint32_t reg[2];
} arm_tcm_mem;

void arm_tcm_init(ARMCPU *cpu, arm_tcm_mem *s);

#endif
