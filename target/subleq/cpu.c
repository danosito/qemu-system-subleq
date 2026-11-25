/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/qemu-print.h"
#include "cpu.h"
#include "accel/tcg/cpu-ops.h"
#include "exec/cputlb.h"
#include "exec/cpu-interrupt.h"
#include "system/memory.h"
#include "exec/page-protection.h"
#include "exec/target_page.h"
#include "hw/core/sysemu-cpu-ops.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-properties-system.h"
#include "migration/vmstate.h"
#include "qemu/main-loop.h"
#include "qemu/timer.h"
#include "system/address-spaces.h"

#define SUBLEQ_IDLE_TICK_NS (NANOSECONDS_PER_SECOND / 100)

/* The host-clock deadline exists only while the vCPU is halted. */
static void subleq_idle_tick(void *opaque)
{
    SubleqCPU *cpu = opaque;
    CPUState *cs = CPU(cpu);
    uint32_t handler;

    if (!cs->halted || !cpu->idle_requested) {
        return;
    }
    handler = address_space_ldl_le(&address_space_memory, SUBLEQ_IRQ_HANDLER,
                                   MEMTXATTRS_UNSPECIFIED, NULL);
    if (handler) {
        address_space_stl_le(&address_space_memory, SUBLEQ_IRQ_SAVED_PC,
                             cpu->env.pc, MEMTXATTRS_UNSPECIFIED, NULL);
        cpu->env.pc = (int32_t)handler / 4 * 4;
        cpu->env.timer = 0;
    }
    cpu->idle_requested = false;
    cpu->idle_timer_wakes++;
    cpu_interrupt(cs, CPU_INTERRUPT_HARD);
}

/* Called with BQL held, including from the virtio IRQ sink. */
void subleq_idle_notify(SubleqCPU *cpu)
{
    if (cpu && cpu->idle_requested) {
        cpu->idle_requested = false;
        cpu->idle_virtio_wakes++;
        timer_del(cpu->idle_timer);
        cpu_interrupt(CPU(cpu), CPU_INTERRUPT_HARD);
    }
}

/* Called after committing the complete SUBLEQ instruction and its next PC. */
void subleq_idle_wait(SubleqCPU *cpu)
{
    CPUState *cs = CPU(cpu);
    uint32_t vector;

    bql_lock();
    vector = address_space_ldl_le(&address_space_memory, SUBLEQ_IRQ_HANDLER,
                                  MEMTXATTRS_UNSPECIFIED, NULL);
    if (vector && (!cpu->virtio_irq_levels || !*cpu->virtio_irq_levels)) {
        cpu->idle_requested = true;
        cpu->idle_waits++;
        cs->halted = true;
        cs->exception_index = EXCP_HLT;
        timer_mod(cpu->idle_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_HOST) + SUBLEQ_IDLE_TICK_NS *
                  (cpu->idle_tick_coalesce_50ms ? 5 : 1));
        bql_unlock();
        cpu_loop_exit(cs);
    }
    bql_unlock();
}

static void subleq_cpu_set_pc(CPUState *cs, vaddr pc)
{
    cpu_env(cs)->pc = pc;
}

static vaddr subleq_cpu_get_pc(CPUState *cs)
{
    return cpu_env(cs)->pc;
}

static TCGTBCPUState subleq_get_tb_cpu_state(CPUState *cs)
{
    return (TCGTBCPUState) { .pc = cpu_env(cs)->pc };
}

static void subleq_restore_state_to_opc(CPUState *cs,
                                       const TranslationBlock *tb,
                                       const uint64_t *data)
{
    cpu_env(cs)->pc = data[0];
}

static void subleq_cpu_dump_state(CPUState *cs, FILE *f, int flags)
{
    CPUSubleqState *env = cpu_env(cs);

    qemu_fprintf(f, "PC=%08" PRIx32 " TIMER=%" PRIu32 "\n",
                 env->pc, env->timer);
    if (SUBLEQ_CPU(cs)->idle_wakeup_v1) {
        SubleqCPU *cpu = SUBLEQ_CPU(cs);

        qemu_fprintf(f, "IDLE_WAITS=%" PRIu64 " TIMER_WAKES=%" PRIu64
                     " VIRTIO_WAKES=%" PRIu64 "\n", cpu->idle_waits,
                     cpu->idle_timer_wakes, cpu->idle_virtio_wakes);
    }
}

static bool subleq_cpu_has_work(CPUState *cs)
{
    /* The ESI instruction counter vectors directly, without a CPU IRQ. */
    return cpu_test_interrupt(cs, CPU_INTERRUPT_HARD);
}

static bool subleq_cpu_exec_interrupt(CPUState *cs, int interrupt_request)
{
    if (interrupt_request & CPU_INTERRUPT_HARD) {
        cpu_reset_interrupt(cs, CPU_INTERRUPT_HARD);
    }
    return false;
}

static int subleq_cpu_mmu_index(CPUState *cs, bool ifetch)
{
    return 0;
}

static bool subleq_cpu_tlb_fill(CPUState *cs, vaddr addr, int size,
                                MMUAccessType access_type, int mmu_idx,
                                bool probe, uintptr_t retaddr)
{
    vaddr page = addr & TARGET_PAGE_MASK;

    tlb_set_page(cs, page, page, PAGE_READ | PAGE_WRITE | PAGE_EXEC,
                 mmu_idx, TARGET_PAGE_SIZE);
    return true;
}

static hwaddr subleq_cpu_get_phys_page_debug(CPUState *cs, vaddr addr)
{
    if (addr < SUBLEQ_RAM_SIZE ||
        (addr <= UINT32_MAX && subleq_in_rom(SUBLEQ_CPU(cs), addr, 1))) {
        return addr & TARGET_PAGE_MASK;
    }
    return -1;
}

static int subleq_console_can_read(void *opaque)
{
    SubleqCPU *cpu = opaque;

    return sizeof(cpu->input) - cpu->input_len;
}

static void subleq_console_read(void *opaque, const uint8_t *buf, int size)
{
    SubleqCPU *cpu = opaque;
    unsigned tail = (cpu->input_head + cpu->input_len) % sizeof(cpu->input);
    unsigned count = MIN(size, subleq_console_can_read(cpu));
    unsigned first = MIN(count, sizeof(cpu->input) - tail);

    memcpy(cpu->input + tail, buf, first);
    memcpy(cpu->input, buf + first, count - first);
    cpu->input_len += count;
}

static void subleq_cpu_reset_hold(Object *obj, ResetType type)
{
    SubleqCPU *cpu = SUBLEQ_CPU(obj);
    SubleqCPUClass *scc = SUBLEQ_CPU_GET_CLASS(cpu);

    if (scc->parent_phases.hold) {
        scc->parent_phases.hold(obj, type);
    }
    cpu->env = (CPUSubleqState) { .pc = cpu->reset_vector };
    cpu->idle_requested = false;
    cpu->idle_waits = cpu->idle_timer_wakes = cpu->idle_virtio_wakes = 0;
    if (cpu->idle_timer) {
        timer_del(cpu->idle_timer);
    }
    memset(cpu->ram_tb_history, 0, sizeof(cpu->ram_tb_history));
    cpu->input_head = 0;
    cpu->input_len = 0;
}

static ObjectClass *subleq_cpu_class_by_name(const char *model)
{
    if (!strcmp(model, "subleq") || !strcmp(model, TYPE_SUBLEQ_CPU)) {
        return object_class_by_name(TYPE_SUBLEQ_CPU);
    }
    return NULL;
}

static void subleq_cpu_realize(DeviceState *dev, Error **errp)
{
    CPUState *cs = CPU(dev);
    SubleqCPU *cpu = SUBLEQ_CPU(dev);
    SubleqCPUClass *scc = SUBLEQ_CPU_GET_CLASS(cpu);

    if (!cpu_exec_realizefn(cs, errp)) {
        return;
    }
    qemu_chr_fe_set_handlers(&cpu->chr, subleq_console_can_read,
                             subleq_console_read, NULL, NULL, cpu, NULL, true);
    if (cpu->idle_wakeup_v1) {
        cpu->idle_timer = timer_new_ns(QEMU_CLOCK_HOST, subleq_idle_tick, cpu);
    }
    qemu_init_vcpu(cs);
    cpu_reset(cs);
    scc->parent_realize(dev, errp);
}

static const struct SysemuCPUOps subleq_sysemu_ops = {
    .has_work = subleq_cpu_has_work,
    .get_phys_page_debug = subleq_cpu_get_phys_page_debug,
};

static const TCGCPUOps subleq_tcg_ops = {
    .guest_default_memory_order = TCG_MO_ALL,
    .initialize = subleq_translate_init,
    .translate_code = subleq_translate_code,
    .get_tb_cpu_state = subleq_get_tb_cpu_state,
    .restore_state_to_opc = subleq_restore_state_to_opc,
    .mmu_index = subleq_cpu_mmu_index,
    .tlb_fill = subleq_cpu_tlb_fill,
    .pointer_wrap = cpu_pointer_wrap_uint32,
    .cpu_exec_interrupt = subleq_cpu_exec_interrupt,
    .cpu_exec_halt = subleq_cpu_has_work,
    .cpu_exec_reset = cpu_reset,
};

static const VMStateDescription vmstate_subleq_cpu = {
    .name = "subleq-cpu",
    .unmigratable = true,
};

static const Property subleq_cpu_properties[] = {
    DEFINE_PROP_CHR("chardev", SubleqCPU, chr),
    DEFINE_PROP_UINT32("reset-vector", SubleqCPU, reset_vector, 0),
    DEFINE_PROP_BOOL("x-mmio-probe", SubleqCPU, mmio_probe, false),
    DEFINE_PROP_BOOL("x-virtio-mmio", SubleqCPU, virtio_mmio, false),
    DEFINE_PROP_BOOL("x-tools-disk", SubleqCPU, tools_disk, false),
    DEFINE_PROP_BOOL("x-idle-wakeup-v1", SubleqCPU, idle_wakeup_v1, false),
    DEFINE_PROP_BOOL("x-idle-tick-coalesce-50ms", SubleqCPU,
                     idle_tick_coalesce_50ms, false),
    DEFINE_PROP_BOOL("x-direct-c-link", SubleqCPU, direct_c_link, false),
    DEFINE_PROP_BOOL("x-ram-burst", SubleqCPU, ram_burst, false),
    DEFINE_PROP_BOOL("x-retranslation-helper", SubleqCPU,
                     retranslation_helper, false),
};

static void subleq_cpu_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    CPUClass *cc = CPU_CLASS(oc);
    SubleqCPUClass *scc = SUBLEQ_CPU_CLASS(oc);

    device_class_set_parent_realize(dc, subleq_cpu_realize,
                                    &scc->parent_realize);
    resettable_class_set_parent_phases(RESETTABLE_CLASS(oc), NULL,
                                       subleq_cpu_reset_hold, NULL,
                                       &scc->parent_phases);
    device_class_set_props(dc, subleq_cpu_properties);
    dc->vmsd = &vmstate_subleq_cpu;

    cc->class_by_name = subleq_cpu_class_by_name;
    cc->set_pc = subleq_cpu_set_pc;
    cc->get_pc = subleq_cpu_get_pc;
    cc->dump_state = subleq_cpu_dump_state;
    cc->sysemu_ops = &subleq_sysemu_ops;
    cc->tcg_ops = &subleq_tcg_ops;
}

static const TypeInfo subleq_cpu_types[] = {
    {
        .name = TYPE_SUBLEQ_CPU,
        .parent = TYPE_CPU,
        .instance_size = sizeof(SubleqCPU),
        .instance_align = __alignof__(SubleqCPU),
        .class_size = sizeof(SubleqCPUClass),
        .class_init = subleq_cpu_class_init,
    },
};

DEFINE_TYPES(subleq_cpu_types)
