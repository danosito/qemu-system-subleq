/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/main-loop.h"
#include "qemu/timer.h"
#include "cpu.h"
#include "accel/tcg/cpu-ldst.h"
#include "exec/target_page.h"
#include "qemu/atomic.h"
#include "exec/cpu-common.h"
#include "exec/helper-proto.h"
#include "exec/translation-block.h"
#include "system/runstate.h"

#define SUBLEQ_IO (-4)

static G_NORETURN void subleq_stop(CPUSubleqState *env, ShutdownCause cause,
                                   int code)
{
    CPUState *cs = env_cpu(env);

    bql_lock();
    qemu_system_shutdown_request_with_code(cause, code);
    bql_unlock();
    cs->halted = true;
    cs->exception_index = EXCP_HLT;
    cpu_loop_exit(cs);
}

static G_NORETURN void subleq_bad_address(CPUSubleqState *env, uint32_t addr)
{
    error_report("ESI: invalid address 0x%08" PRIx32 " at PC 0x%08" PRIx32,
                 addr, env->pc);
    subleq_stop(env, SHUTDOWN_CAUSE_GUEST_PANIC, 1);
}

static inline void subleq_check_ram(CPUSubleqState *env, uint32_t addr)
{
    if (unlikely(addr >= SUBLEQ_RAM_SIZE)) {
        subleq_bad_address(env, addr);
    }
}

static inline void subleq_check_read(CPUSubleqState *env, uint32_t addr,
                                     bool allow_mmio)
{
    SubleqCPU *cpu = env_archcpu(env);

    if (unlikely(addr >= SUBLEQ_RAM_SIZE) &&
        !subleq_in_rom(cpu, addr, 4) &&
        !(allow_mmio && cpu->virtio_mmio &&
          (addr - ESI_VIRTIO_MMIO_READ <=
           (cpu->tools_disk ? 4 : 3) * ESI_VIRTIO_MMIO_STRIDE - 4 ||
           addr - ESI_VIRTIO_MMIO_WRITE <=
           (cpu->tools_disk ? 4 : 3) * ESI_VIRTIO_MMIO_STRIDE - 4 ||
           addr == ESI_VIRTIO_MMIO_PENDING)) &&
        !(allow_mmio && cpu->idle_wakeup_v1 &&
          (addr == ESI_IDLE_WAKEUP_WAIT ||
           addr == ESI_IDLE_WAKEUP_VERSION)) &&
        !(allow_mmio && cpu->mmio_probe &&
          (addr - ESI_MMIO_PROBE_READ <= ESI_MMIO_PROBE_READ_SIZE - 4 ||
           addr == ESI_MMIO_PROBE_WRITE))) {
        subleq_bad_address(env, addr);
    }
}

static inline void subleq_check_subtract_dest(CPUSubleqState *env,
                                              uint32_t addr)
{
    SubleqCPU *cpu = env_archcpu(env);

    if (unlikely(addr >= SUBLEQ_RAM_SIZE) &&
        !(cpu->virtio_mmio &&
          addr - ESI_VIRTIO_MMIO_WRITE <=
           (cpu->tools_disk ? 4 : 3) * ESI_VIRTIO_MMIO_STRIDE - 4) &&
        !(cpu->idle_wakeup_v1 && addr == ESI_IDLE_WAKEUP_WAIT) &&
        !(cpu->mmio_probe && addr == ESI_MMIO_PROBE_WRITE)) {
        subleq_bad_address(env, addr);
    }
}

/* Match the reference VM's signed division to word indices. */
static inline int32_t subleq_word_address(int32_t addr)
{
    return addr / 4 * 4;
}

static inline int32_t subleq_operand(CPUSubleqState *env, uint32_t pc,
                                     uintptr_t ra)
{
    int32_t raw = cpu_ldl_le_mmuidx_ra(env, pc, 0, ra);
    int32_t addr = subleq_word_address(raw);

    if (raw & 1) {
        subleq_check_read(env, addr, false);
        addr = subleq_word_address(cpu_ldl_le_mmuidx_ra(env, addr, 0, ra));
    }
    return addr;
}

static void subleq_getchar(CPUSubleqState *env, uint32_t addr, uintptr_t ra)
{
    SubleqCPU *cpu = SUBLEQ_CPU(env_cpu(env));
    int value = -1;

    bql_lock();
    if (cpu->input_len) {
        value = cpu->input[cpu->input_head];
        cpu->input_head = (cpu->input_head + 1) % sizeof(cpu->input);
        cpu->input_len--;
        qemu_chr_fe_accept_input(&cpu->chr);
    }
    bql_unlock();

    /* Empty polls write nothing; input bytes do not change the high bits. */
    if (value >= 0) {
        cpu_stb_mmuidx_ra(env, addr, value, 0, ra);
    }
}

static void subleq_putchar(CPUSubleqState *env, uint32_t addr, uintptr_t ra)
{
    SubleqCPU *cpu = SUBLEQ_CPU(env_cpu(env));
    uint8_t byte = cpu_ldub_mmuidx_ra(env, addr, 0, ra);

    bql_lock();
    qemu_chr_fe_write_all(&cpu->chr, &byte, 1);
    bql_unlock();
}

static void subleq_update_clock(CPUSubleqState *env, uintptr_t ra)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_HOST);
    uint64_t seconds = now / NANOSECONDS_PER_SECOND;

    cpu_stl_le_mmuidx_ra(env, SUBLEQ_CLOCK, seconds, 0, ra);
    cpu_stl_le_mmuidx_ra(env, SUBLEQ_CLOCK + 4, seconds >> 32, 0, ra);
    cpu_stl_le_mmuidx_ra(env, SUBLEQ_CLOCK + 8, now % NANOSECONDS_PER_SECOND,
                         0, ra);
    /* The reference VM writes a 64-bit tv_nsec. */
    cpu_stl_le_mmuidx_ra(env, SUBLEQ_CLOCK + 12, 0, 0, ra);
}

static uint32_t subleq_timer_irq(CPUSubleqState *env, uint32_t pc,
                                 uint32_t handler, uintptr_t ra)
{
    env->timer = 0;
    cpu_stl_le_mmuidx_ra(env, SUBLEQ_IRQ_SAVED_PC, pc, 0, ra);
    return subleq_word_address(handler);
}

uint32_t HELPER(subleq_timer_irq)(CPUSubleqState *env, uint32_t pc,
                                  uint32_t handler)
{
    return subleq_timer_irq(env, pc, handler, GETPC());
}

uint32_t HELPER(subleq_burst_live)(void *tb)
{
    return !(tb_cflags(tb) & CF_INVALID);
}

/* Called only after a completed subtract and a nonzero vector read. */
void HELPER(subleq_burst_tick)(CPUSubleqState *env, uint32_t pc,
                               uint32_t handler)
{
    if (env->timer++ > SUBLEQ_TIMER_THRESHOLD) {
        pc = subleq_timer_irq(env, pc, handler, GETPC());
    }
    env->pc = pc;
}

/* Mirror the TCG qemu_ld/st direct-hit test: equality rejects MMIO,
 * watchpoints, write-to-clean-code (NOTDIRTY), and all FORCE_SLOW flags.
 * Every call takes a fresh TLB entry: a prior access or interrupt may have
 * invalidated the page or TLB. A miss goes through the normal checked MMU
 * routine with the helper's GETPC as the restore address. */
static inline uint32_t subleq_compact_load(CPUSubleqState *env, uint32_t addr,
                                           uintptr_t ra, bool after_ram_read,
                                           bool *was_ram_read)
{
    CPUState *cs = env_cpu(env);
    CPUTLBDescFast *fast = cpu_tlb_fast(cs, 0);
    CPUTLBEntry *entry = &fast->table[(addr >> TARGET_PAGE_BITS) &
                                     (fast->mask >> CPU_TLB_ENTRY_BITS)];

    /* SUBLEQ's guest ordering is TCG_MO_ALL; x86 TCG fences ST_LD
     * immediately before each qemu_ld. This includes the first access
     * following an earlier TB's store. */
    if (after_ram_read) {
        /* The immediately preceding access was an aligned direct-hit RAM
         * load following smp_mb(): no intervening guest store, MMU helper,
         * MMIO or instrumentation. On x86 WB RAM only LD_LD remains. Do
         * not carry this proof across a store, slow access or helper call. */
        smp_rmb();
    } else {
        smp_mb();
    }
#ifdef CONFIG_PLUGIN
    if (unlikely(cs->neg.plugin_mem_cbs)) {
        return cpu_ldl_le_mmuidx_ra(env, addr, 0, ra);
    }
#endif
    if (likely(qatomic_read(&entry->addr_read) ==
               (addr & (TARGET_PAGE_MASK | 3)))) {
        if (was_ram_read) {
            *was_ram_read = true;
        }
        return ldl_le_p((void *)(uintptr_t)(addr + entry->addend));
    }
    return cpu_ldl_le_mmuidx_ra(env, addr, 0, ra);
}

static inline uint32_t subleq_compact_ld(CPUSubleqState *env, uint32_t addr,
                                         uintptr_t ra)
{
    return subleq_compact_load(env, addr, ra, false, NULL);
}

static inline uint32_t subleq_read_difference(CPUSubleqState *env, uint32_t a,
                                              uint32_t b, uintptr_t ra, bool pair)
{
    bool direct = false;
    uint32_t lhs = subleq_compact_load(env, b, ra, false, pair ? &direct : NULL);
    return lhs - subleq_compact_load(env, a, ra, pair && direct, NULL);
}

static inline void subleq_compact_st(CPUSubleqState *env, uint32_t addr,
                                     uint32_t value, uintptr_t ra)
{
    CPUState *cs = env_cpu(env);
    CPUTLBDescFast *fast = cpu_tlb_fast(cs, 0);
    CPUTLBEntry *entry = &fast->table[(addr >> TARGET_PAGE_BITS) &
                                     (fast->mask >> CPU_TLB_ENTRY_BITS)];

#ifdef CONFIG_PLUGIN
    if (unlikely(cs->neg.plugin_mem_cbs)) {
        cpu_stl_le_mmuidx_ra(env, addr, value, 0, ra);
        return;
    }
#endif
    if (likely(qatomic_read(&entry->addr_write) ==
               (addr & (TARGET_PAGE_MASK | 3)))) {
        stl_le_p((void *)(uintptr_t)(addr + entry->addend), value);
    } else {
        cpu_stl_le_mmuidx_ra(env, addr, value, 0, ra);
    }
}

/* One shared checked body for decoded, direct, ordinary RAM triples.
 * The translator retains tracked code fetches, insn_start, watchpoint
 * fallback and static RAM bounds. No host address survives an access. */
static inline void subleq_compact(CPUSubleqState *env, uint32_t a, uint32_t b,
                                  uint32_t c, uint32_t pc, uintptr_t ra, bool pair)
{
    uint32_t result, handler;

    result = subleq_read_difference(env, a, b, ra, pair);
    subleq_compact_st(env, b, result, ra);
    pc = (int32_t)result <= 0 ? c : pc + SUBLEQ_INSN_SIZE;
    handler = subleq_compact_ld(env, SUBLEQ_IRQ_HANDLER, ra);
    if (handler && env->timer++ > SUBLEQ_TIMER_THRESHOLD) {
        pc = subleq_timer_irq(env, pc, handler, ra);
    }
    env->pc = pc;
}

/* Fallthrough and branch target coincide. Constant page index and alignment
 * let the compiler simplify the existing checked accessors without assuming
 * a hit, retaining a host pointer, or dropping either architectural read. */
static inline void subleq_compact_low_seq(CPUSubleqState *env, uint32_t ab,
                                          uint32_t next, uintptr_t ra, bool pair)
{
    uint32_t a = ab & 0xffc, b = (ab >> 12) & 0xffc;
    uint32_t result = subleq_read_difference(env, a, b, ra, pair);
    uint32_t handler;

    subleq_compact_st(env, b, result, ra);
    handler = subleq_compact_ld(env, SUBLEQ_IRQ_HANDLER, ra);
    if (handler && env->timer++ > SUBLEQ_TIMER_THRESHOLD) {
        next = subleq_timer_irq(env, next, handler, ra);
    }
    env->pc = next;
}

void HELPER(subleq_compact)(CPUSubleqState *env, uint32_t a, uint32_t b,
                            uint32_t c, uint32_t pc)
{
    subleq_compact(env, a, b, c, pc, GETPC(), false);
}

void HELPER(subleq_compact_pair)(CPUSubleqState *env, uint32_t a, uint32_t b,
                                 uint32_t c, uint32_t pc)
{
    subleq_compact(env, a, b, c, pc, GETPC(), true);
}

void HELPER(subleq_compact_low_seq)(CPUSubleqState *env, uint32_t ab,
                                    uint32_t next)
{
    subleq_compact_low_seq(env, ab, next, GETPC(), false);
}

void HELPER(subleq_compact_pair_low_seq)(CPUSubleqState *env, uint32_t ab,
                                         uint32_t next)
{
    subleq_compact_low_seq(env, ab, next, GETPC(), true);
}

void HELPER(subleq_step)(CPUSubleqState *env)
{
    uintptr_t ra = GETPC();
    uint32_t pc = env->pc;
    int32_t a, b, c;
    bool timer_irq_taken = false;

    if (unlikely(pc % 4 ||
                 (pc > SUBLEQ_RAM_SIZE - SUBLEQ_INSN_SIZE &&
                  !subleq_in_rom(env_archcpu(env), pc, SUBLEQ_INSN_SIZE)))) {
        subleq_bad_address(env, pc);
    }
    a = subleq_operand(env, pc, ra);
    b = subleq_operand(env, pc + 4, ra);
    c = subleq_operand(env, pc + 8, ra);
    pc += SUBLEQ_INSN_SIZE;

    if (a == SUBLEQ_IO) {
        if (c == SUBLEQ_IO) {
            int status;

            /* Explicit halt encoding described by the ESI architecture. */
            subleq_check_read(env, b, false);
            status = cpu_ldub_mmuidx_ra(env, b, 0, ra);
            subleq_stop(env, SHUTDOWN_CAUSE_GUEST_SHUTDOWN, status);
        }
        subleq_check_ram(env, b);
        subleq_getchar(env, b, ra);
    } else if (b == SUBLEQ_IO) {
        subleq_check_read(env, a, false);
        subleq_putchar(env, a, ra);
    } else {
        uint32_t result, handler;

        subleq_check_read(env, a, true);
        subleq_check_subtract_dest(env, b);
        if (a == SUBLEQ_CLOCK) {
            subleq_update_clock(env, ra);
        }
        result = cpu_ldl_le_mmuidx_ra(env, b, 0, ra);
        result -= cpu_ldl_le_mmuidx_ra(env, a, 0, ra);
        cpu_stl_le_mmuidx_ra(env, b, result, 0, ra);
        if ((int32_t)result <= 0) {
            pc = c;
        }
        /* Count even taken branches, but not console operations. */
        handler = cpu_ldl_le_mmuidx_ra(env, SUBLEQ_IRQ_HANDLER, 0, ra);
        if (handler && env->timer++ > SUBLEQ_TIMER_THRESHOLD) {
            pc = subleq_timer_irq(env, pc, handler, ra);
            timer_irq_taken = true;
        }
    }

    env->pc = pc;
    if (!c) {
        subleq_stop(env, SHUTDOWN_CAUSE_GUEST_SHUTDOWN, 0);
    }
    if (b == ESI_IDLE_WAKEUP_WAIT && env_archcpu(env)->idle_wakeup_v1 &&
        !timer_irq_taken && a != SUBLEQ_IO) {
        subleq_idle_wait(env_archcpu(env));
    }
}
