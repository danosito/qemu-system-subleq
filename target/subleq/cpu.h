/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SUBLEQ_CPU_H
#define SUBLEQ_CPU_H

#include "chardev/char-fe.h"
#include "exec/cpu-defs.h"
#include "hw/core/cpu.h"
#include "qemu/units.h"

#define TYPE_SUBLEQ_CPU "subleq-cpu"
#define CPU_RESOLVING_TYPE TYPE_SUBLEQ_CPU
OBJECT_DECLARE_CPU_TYPE(SubleqCPU, SubleqCPUClass, SUBLEQ_CPU)

#define SUBLEQ_RAM_SIZE (1536 * MiB)
#define SUBLEQ_INSN_SIZE 12
#define SUBLEQ_CLOCK 0x100
#define SUBLEQ_IRQ_HANDLER 0x00
#define SUBLEQ_IRQ_SAVED_PC 0x04
#define SUBLEQ_TIMER_THRESHOLD 800000
#define ESI_ROM_BASE 0x7f000000u
#define ESI_ROM_SIZE (64 * KiB)
#define ESI_MMIO_PROBE_READ 0x7f010000u
#define ESI_MMIO_PROBE_READ_SIZE 8
#define ESI_MMIO_PROBE_WRITE 0x7f011000u
#define ESI_VIRTIO_MMIO_READ 0x60000000u
#define ESI_VIRTIO_MMIO_WRITE 0x61000000u
#define ESI_VIRTIO_MMIO_PENDING 0x62000000u
#define ESI_IDLE_WAKEUP_WAIT 0x63000000u
#define ESI_IDLE_WAKEUP_VERSION 0x63000004u
#define ESI_VIRTIO_MMIO_STRIDE 0x1000u
#define ESI_VIRTIO_MMIO_SLOTS 4
#define ESI_VIRTIO_MMIO_SIZE (ESI_VIRTIO_MMIO_SLOTS * ESI_VIRTIO_MMIO_STRIDE)

typedef struct CPUArchState {
    uint32_t pc;                 /* Byte address. */
    uint32_t timer;
} CPUSubleqState;

typedef struct SubleqTBHistory {
    uint32_t pc;
    unsigned translations;
} SubleqTBHistory;

struct ArchCPU {
    CPUState parent_obj;
    CPUSubleqState env;
    uint32_t reset_vector;
    bool mmio_probe;
    bool virtio_mmio;
    bool tools_disk;
    bool idle_wakeup_v1;
    bool idle_tick_coalesce_50ms;
    uint32_t *virtio_irq_levels; /* Board-owned, accessed under BQL. */
    QEMUTimer *idle_timer;
    bool idle_requested;
    uint64_t idle_waits;
    uint64_t idle_timer_wakes;
    uint64_t idle_virtio_wakes;
    bool ram_burst;
    bool direct_c_link;
    bool compact_tb;
    bool ordered_read_pair;
    bool compact_tcg;
    bool retranslation_helper;
    /* Translation policy only; not guest CPU state. */
    uint32_t translated_run_start;
    uint32_t translated_run_next;
    SubleqTBHistory ram_tb_history[64];
    CharFrontend chr;
    /* Accessed under the BQL by the frontend and the CPU helper. */
    uint8_t input[256];
    unsigned input_head;
    unsigned input_len;
};

struct SubleqCPUClass {
    CPUClass parent_class;
    DeviceRealize parent_realize;
    ResettablePhases parent_phases;
};

static inline bool subleq_in_rom(SubleqCPU *cpu, uint32_t addr, size_t len)
{
    return len <= ESI_ROM_SIZE && addr - ESI_ROM_BASE <= ESI_ROM_SIZE - len &&
           cpu->reset_vector == ESI_ROM_BASE;
}

void subleq_idle_wait(SubleqCPU *cpu);
void subleq_idle_notify(SubleqCPU *cpu);
void subleq_translate_init(void);
void subleq_translate_code(CPUState *cs, TranslationBlock *tb,
                           int *max_insns, vaddr pc, void *host_pc);

#endif
