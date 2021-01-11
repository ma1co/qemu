/* ARM tightly-coupled memory implementation */

#include "qemu/osdep.h"
#include "arm-tcm.h"
#include "qapi/error.h"

static void arm_tcm_mem_update(CPUARMState *env, arm_tcm_mem *s, int i)
{
    ARMCPU *cpu = env_archcpu(env);
    MemoryRegion *memory = CPU(cpu)->memory;

    if (memory_region_is_mapped(s->mr[i])) {
        memory_region_del_subregion(memory, s->mr[i]);
    }
    if (s->reg[i] & 1) {
        memory_region_add_subregion_overlap(memory, s->reg[i] & 0xfffff000, s->mr[i], 1);
    }
}

static uint64_t arm_tcm_mem_read(CPUARMState *env, const ARMCPRegInfo *ri)
{
    arm_tcm_mem *s = (arm_tcm_mem *) ri->opaque;
    int i = ri->opc2;

    return s->reg[i];
}

static void arm_tcm_mem_write(CPUARMState *env, const ARMCPRegInfo *ri, uint64_t value)
{
    arm_tcm_mem *s = (arm_tcm_mem *) ri->opaque;
    int i = ri->opc2;

    s->reg[i] = value;
    arm_tcm_mem_update(env, s, i);
}

static void arm_tcm_mem_reset(CPUARMState *env, const ARMCPRegInfo *ri)
{
    arm_tcm_mem *s = (arm_tcm_mem *) ri->opaque;
    int i = ri->opc2;

    s->reg[i] = 0xc;
    arm_tcm_mem_update(env, s, i);
}

static const ARMCPRegInfo arm_tcm_mem_cp_reginfo[] = {
    { .name = "TCMDR", .cp = 15, .opc1 = 0, .crn = 9, .crm = 1, .opc2 = 0,
      .access = PL1_RW, .type = ARM_CP_IO,
      .readfn = arm_tcm_mem_read, .writefn = arm_tcm_mem_write, .resetfn = arm_tcm_mem_reset },
    { .name = "TCMIR", .cp = 15, .opc1 = 0, .crn = 9, .crm = 1, .opc2 = 1,
      .access = PL1_RW, .type = ARM_CP_IO,
      .readfn = arm_tcm_mem_read, .writefn = arm_tcm_mem_write, .resetfn = arm_tcm_mem_reset },
    REGINFO_SENTINEL
};

void arm_tcm_init(ARMCPU *cpu, arm_tcm_mem *s)
{
    int i;
    for (i = 0; i < 2; i++) {
        s->mr[i] = g_new(MemoryRegion, 1);
        memory_region_init_ram(s->mr[i], NULL, g_strdup_printf("tcm%d", i), 0x1000, &error_fatal);
    }
    define_arm_cp_regs_with_opaque(cpu, arm_tcm_mem_cp_reginfo, s);
}
