/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "cpu.h"
#include "tcg/tcg-op.h"
#include "exec/helper-proto.h"
#include "exec/helper-gen.h"
#include "exec/translation-block.h"
#include "exec/translator.h"
#include "exec/target_page.h"

#define HELPER_H "helper.h"
#include "exec/helper-info.c.inc"
#undef HELPER_H

#define DISAS_RETRANSLATED DISAS_TARGET_0
#define DISAS_BURST_DONE DISAS_TARGET_1
#define SUBLEQ_BURST_INSNS 8

typedef struct SubleqDisasContext {
    DisasContextBase base;
    unsigned burst_len;
    unsigned compact_len;
    TCGLabel *compact_exit;
    bool direct_c_link;
    bool ordered_read_pair;
    uint32_t words[SUBLEQ_BURST_INSNS][3];
} SubleqDisasContext;

void subleq_translate_init(void)
{
}

static void subleq_tr_init_disas_context(DisasContextBase *db, CPUState *cs)
{
}

static void subleq_tr_tb_start(DisasContextBase *db, CPUState *cs)
{
}

static void subleq_tr_insn_start(DisasContextBase *db, CPUState *cs)
{
    tcg_gen_insn_start(db->pc_next);
}

static TCGv_i32 subleq_gen_ram_operand(uint32_t pc, TCGLabel *slow)
{
    TCGv_i32 raw = tcg_temp_new_i32();
    TCGv_i32 addr = tcg_temp_new_i32();
    TCGLabel *direct = gen_new_label();

    tcg_gen_qemu_ld_i32(raw, tcg_constant_tl(pc), 0, MO_LEUL | MO_ALIGN);
    /* RAM addresses are non-negative, so truncation just clears two bits. */
    tcg_gen_brcondi_i32(TCG_COND_GEU, raw, SUBLEQ_RAM_SIZE, slow);
    tcg_gen_andi_i32(addr, raw, -4);
    tcg_gen_brcondi_i32(TCG_COND_TSTEQ, raw, 1, direct);
    tcg_gen_qemu_ld_i32(addr, addr, 0, MO_LEUL | MO_ALIGN);
    tcg_gen_brcondi_i32(TCG_COND_GEU, addr, SUBLEQ_RAM_SIZE, slow);
    tcg_gen_andi_i32(addr, addr, -4);
    gen_set_label(direct);
    return addr;
}

static void subleq_gen_ram_step(uint32_t pc, TCGv_i32 a, TCGv_i32 b,
                                TCGv_i32 c, TCGLabel *slow, bool short_timer)
{
    TCGv_i32 val = tcg_temp_new_i32();
    TCGv_i32 result = tcg_temp_new_i32();
    TCGv_i32 next = tcg_temp_new_i32();
    TCGv_i32 handler = tcg_temp_new_i32();
    TCGv_i32 timer = tcg_temp_new_i32();
    TCGLabel *done = gen_new_label();

    /* Finish all checks before the first side effect; the helper can retry. */
    tcg_gen_brcondi_i32(TCG_COND_EQ, a, SUBLEQ_CLOCK, slow);
    tcg_gen_brcondi_i32(TCG_COND_EQ, c, 0, slow);
    tcg_gen_qemu_ld_i32(result, b, 0, MO_LEUL | MO_ALIGN);
    tcg_gen_qemu_ld_i32(val, a, 0, MO_LEUL | MO_ALIGN);
    tcg_gen_sub_i32(result, result, val);
    tcg_gen_qemu_st_i32(result, b, 0, MO_LEUL | MO_ALIGN);
    tcg_gen_movcond_i32(TCG_COND_LE, next, result, tcg_constant_i32(0),
                        c, tcg_constant_i32(pc + SUBLEQ_INSN_SIZE));

    /* A store to word zero can change whether this instruction ticks. */
    tcg_gen_qemu_ld_i32(handler, tcg_constant_tl(SUBLEQ_IRQ_HANDLER),
                        0, MO_LEUL | MO_ALIGN);
    tcg_gen_brcondi_i32(TCG_COND_EQ, handler, 0, done);
    if (short_timer) {
        TCGLabel *exit = gen_new_label();
        gen_helper_subleq_burst_tick(tcg_env, next, handler);
        tcg_gen_br(exit);
        gen_set_label(done);
        tcg_gen_st_i32(next, tcg_env, offsetof(CPUSubleqState, pc));
        gen_set_label(exit);
    } else {
        tcg_gen_ld_i32(timer, tcg_env, offsetof(CPUSubleqState, timer));
        tcg_gen_addi_i32(val, timer, 1);
        tcg_gen_st_i32(val, tcg_env, offsetof(CPUSubleqState, timer));
        tcg_gen_brcondi_i32(TCG_COND_LEU, timer, SUBLEQ_TIMER_THRESHOLD, done);
        gen_helper_subleq_timer_irq(next, tcg_env, next, handler);
        gen_set_label(done);
        tcg_gen_st_i32(next, tcg_env, offsetof(CPUSubleqState, pc));
    }
}

static bool subleq_decode_ram_triple(DisasContextBase *db, CPUState *cs,
                                     uint32_t words[3])
{
    vaddr pc = db->pc_next;

    /* Specialize one complete triple in QEMU's tracked RAM page only. */
    if (!db->host_addr[0] || db->plugin_enabled ||
        !translator_is_same_page(db, pc + SUBLEQ_INSN_SIZE - 1)) {
        return false;
    }
    for (unsigned i = 0; i < 3; i++) {
        uint32_t raw = translator_ldl_end(cpu_env(cs), db, pc + 4 * i, MO_LE);

        if (raw >= SUBLEQ_RAM_SIZE) {
            return false;
        }
        words[i] = raw;
    }
    return true;
}

static TCGv_i32 subleq_gen_decoded_operand(uint32_t raw, TCGLabel *slow)
{
    TCGv_i32 base = tcg_constant_i32(raw & -4u);
    TCGv_i32 addr;

    if (!(raw & 1)) {
        return base;
    }
    /* Code fixes the pointer location, not the value stored there. */
    addr = tcg_temp_new_i32();
    tcg_gen_qemu_ld_i32(addr, base, 0, MO_LEUL | MO_ALIGN);
    tcg_gen_brcondi_i32(TCG_COND_GEU, addr, SUBLEQ_RAM_SIZE, slow);
    tcg_gen_andi_i32(addr, addr, -4);
    return addr;
}

static bool subleq_use_retranslation_helper(DisasContextBase *db,
                                             CPUState *cs, uint32_t pc)
{
    SubleqCPU *cpu = SUBLEQ_CPU(cs);
    SubleqTBHistory *entry;

    /* Debug and forced single-instruction replays do not train this policy. */
    if (!cpu->retranslation_helper || cpu->compact_tb ||
        (tb_cflags(db->tb) & (CF_NO_GOTO_TB | CF_NOIRQ | CF_COUNT_MASK))) {
        return false;
    }
    entry = &cpu->ram_tb_history[(pc / 4) % ARRAY_SIZE(cpu->ram_tb_history)];
    if (entry->pc != pc) {
        *entry = (SubleqTBHistory) { .pc = pc };
    }

    /* Limit code-generation work for a repeatedly translated RAM PC. */
    if (entry->translations >= 3) {
        return true;
    }
    entry->translations++;
    return false;
}

/*
 * Translate a bounded RAM region, not a loop hidden inside a helper. Each
 * triple still gets QEMU's insn_start, restore point and tracked code bytes.
 * Only a final instruction may transfer control non-sequentially.
 */
static unsigned subleq_plan_burst(SubleqDisasContext *dc, CPUState *cs,
                                  bool compact)
{
    DisasContextBase *db = &dc->base;
    vaddr pc = db->pc_first;
    unsigned n = 0;
    bool rom = compact && pc <= UINT32_MAX &&
               subleq_in_rom(SUBLEQ_CPU(cs), pc, SUBLEQ_INSN_SIZE);

    if (!db->host_addr[0] || db->plugin_enabled || (pc & 3) ||
        (compact ? (tb_cflags(db->tb) & ~CF_CLUSTER_MASK) != 0 :
         SUBLEQ_CPU(cs)->retranslation_helper) ||
        (tb_cflags(db->tb) & (CF_COUNT_MASK | CF_SINGLE_STEP | CF_BP_PAGE |
                              CF_NO_GOTO_TB | CF_NO_GOTO_PTR | CF_USE_ICOUNT |
                              CF_NOIRQ | CF_MEMI_ONLY))) {
        return 0;
    }
    while (n < MIN(SUBLEQ_BURST_INSNS, db->max_insns) &&
           (pc <= SUBLEQ_RAM_SIZE - SUBLEQ_INSN_SIZE ||
            (rom && subleq_in_rom(SUBLEQ_CPU(cs), pc, SUBLEQ_INSN_SIZE))) &&
           translator_is_same_page(db, pc + SUBLEQ_INSN_SIZE - 1)) {
        uint32_t *words = dc->words[n];
        for (int i = 0; i < 3; i++) {
            uint32_t raw = translator_ldl_end(cpu_env(cs), db, pc + i * 4,
                                              MO_LE);
            /* Only C is control, never a data pointer. ROM code fetch is
             * tracked like RAM; MMIO, indirects, ROM data and stores to ROM
             * remain on the unmodified reference path. */
            if ((raw & 1) || (raw >= SUBLEQ_RAM_SIZE &&
                !(rom && i == 2 && subleq_in_rom(SUBLEQ_CPU(cs), raw & -4u,
                                                 SUBLEQ_INSN_SIZE)))) {
                return n;
            }
            words[i] = raw & -4u;
        }
        if (words[0] == SUBLEQ_CLOCK || !words[2] ||
            (!compact && words[1] < TARGET_PAGE_SIZE) ||
            translator_is_same_page(db, words[1])) {
            break;
        }
        n++;
        pc += SUBLEQ_INSN_SIZE;
        if (words[2] != pc) {
            break;
        }
    }
    return n;
}

static void subleq_burst_exit(DisasContextBase *db, uint32_t dest, int slot)
{
    tcg_gen_st_i32(tcg_constant_i32(dest), tcg_env, offsetof(CPUSubleqState, pc));
    if (translator_use_goto_tb(db, dest)) {
        tcg_gen_goto_tb(slot);
        tcg_gen_exit_tb(db->tb, slot);
    } else {
        tcg_gen_lookup_and_goto_ptr();
    }
}

static void subleq_gen_burst_insn(SubleqDisasContext *dc)
{
    DisasContextBase *db = &dc->base;
    uint32_t *words = dc->words[db->num_insns - 1];
    uint32_t fallthrough = db->pc_next + SUBLEQ_INSN_SIZE;
    TCGv_i32 a = tcg_temp_new_i32();
    TCGv_i32 result = tcg_temp_new_i32();
    TCGv_i32 handler = tcg_temp_new_i32();
    TCGv_i32 next = tcg_temp_new_i32();
    TCGLabel *no_timer = gen_new_label();

    if (db->num_insns == 1) {
        TCGLabel *unwatched = gen_new_label();
        TCGv_ptr wp = tcg_temp_new_ptr();
        ptrdiff_t off = offsetof(CPUState, watchpoints.tqh_first);

        tcg_gen_ld_ptr(wp, tcg_env, off - (ptrdiff_t)offsetof(SubleqCPU, env));
        tcg_gen_brcondi_ptr(TCG_COND_EQ, wp, 0, unwatched);
        gen_helper_subleq_step(tcg_env);
        tcg_gen_lookup_and_goto_ptr();
        gen_set_label(unwatched);
    } else {
        TCGLabel *live = gen_new_label();
        TCGv_i32 ok = tcg_temp_new_i32();

        /* Do not run a stale suffix after an external physical code write. */
        gen_helper_subleq_burst_live(ok, tcg_constant_ptr(db->tb));
        tcg_gen_brcondi_i32(TCG_COND_NE, ok, 0, live);
        tcg_gen_exit_tb(NULL, 0);
        gen_set_label(live);
    }
    tcg_gen_qemu_ld_i32(result, tcg_constant_tl(words[1]), 0, MO_LEUL | MO_ALIGN);
    tcg_gen_qemu_ld_i32(a, tcg_constant_tl(words[0]), 0, MO_LEUL | MO_ALIGN);
    tcg_gen_sub_i32(result, result, a);
    tcg_gen_qemu_st_i32(result, tcg_constant_tl(words[1]), 0, MO_LEUL | MO_ALIGN);
    tcg_gen_qemu_ld_i32(handler, tcg_constant_tl(SUBLEQ_IRQ_HANDLER), 0,
                        MO_LEUL | MO_ALIGN);
    tcg_gen_brcondi_i32(TCG_COND_EQ, handler, 0, no_timer);
    tcg_gen_movcond_i32(TCG_COND_LE, next, result, tcg_constant_i32(0),
                        tcg_constant_i32(words[2]), tcg_constant_i32(fallthrough));
    gen_helper_subleq_burst_tick(tcg_env, next, handler);
    tcg_gen_lookup_and_goto_ptr();
    gen_set_label(no_timer);

    db->pc_next = fallthrough;
    if (db->num_insns == dc->burst_len) {
        if (words[2] != fallthrough) {
            TCGLabel *positive = gen_new_label();
            tcg_gen_brcondi_i32(TCG_COND_GT, result, 0, positive);
            subleq_burst_exit(db, words[2], 1);
            gen_set_label(positive);
        }
        subleq_burst_exit(db, fallthrough, 0);
        db->is_jmp = DISAS_BURST_DONE;
    } else {
        /* Canonical state if translator_loop hits its op-buffer limit here. */
        tcg_gen_st_i32(tcg_constant_i32(fallthrough), tcg_env,
                       offsetof(CPUSubleqState, pc));
    }
}

/* Link only if the committed PC, including any timer redirect, matches. */
static void subleq_gen_guarded_slot1(DisasContextBase *db, uint32_t dest)
{
    TCGLabel *miss = gen_new_label();
    TCGv_i32 resolved = tcg_temp_new_i32();

    tcg_gen_ld_i32(resolved, tcg_env, offsetof(CPUSubleqState, pc));
    tcg_gen_brcondi_i32(TCG_COND_NE, resolved, dest, miss);
    tcg_gen_goto_tb(1);
    tcg_gen_exit_tb(db->tb, 1);
    gen_set_label(miss);
}

/* A compact region amortizes the TB header, fallback and dispatcher across
 * ordinary direct triples. No guest accesses are fused: the same checked
 * helpers commit each instruction and tick, with their own restore point. */
static void subleq_compact_body(uint32_t pc, const uint32_t words[3], bool pair)
{
    if ((words[0] | words[1]) < 4096 &&
        (words[2] & -4u) == pc + SUBLEQ_INSN_SIZE) {
        uint32_t ab = (words[0] & 0xffc) | ((words[1] & 0xffc) << 12);
        (pair ? gen_helper_subleq_compact_pair_low_seq :
                gen_helper_subleq_compact_low_seq)(tcg_env, tcg_constant_i32(ab),
            tcg_constant_i32(pc + SUBLEQ_INSN_SIZE));
    } else {
        (pair ? gen_helper_subleq_compact_pair : gen_helper_subleq_compact)(
            tcg_env, tcg_constant_i32(words[0] & -4u),
            tcg_constant_i32(words[1] & -4u), tcg_constant_i32(words[2] & -4u),
            tcg_constant_i32(pc));
    }
}

static void subleq_compact_region_insn(SubleqDisasContext *dc)
{
    DisasContextBase *db = &dc->base;
    uint32_t *words = dc->words[db->num_insns - 1];
    ptrdiff_t off = offsetof(CPUState, watchpoints.tqh_first) -
                    (ptrdiff_t)offsetof(SubleqCPU, env);
    TCGv_ptr wp = tcg_temp_new_ptr();

    tcg_gen_ld_ptr(wp, tcg_env, off);
    if (db->num_insns == 1) {
        TCGLabel *fast = gen_new_label();
        dc->compact_exit = gen_new_label();
        tcg_gen_brcondi_ptr(TCG_COND_EQ, wp, 0, fast);
        gen_helper_subleq_step(tcg_env);
        tcg_gen_br(dc->compact_exit);
        gen_set_label(fast);
    } else {
        TCGv_i32 flags = tcg_temp_new_i32();
        TCGv_i32 pc = tcg_temp_new_i32();
        tcg_gen_brcondi_ptr(TCG_COND_NE, wp, 0, dc->compact_exit);
        tcg_gen_ld_i32(pc, tcg_env, offsetof(CPUSubleqState, pc));
        tcg_gen_brcondi_i32(TCG_COND_NE, pc, db->pc_next, dc->compact_exit);
        /* Recheck against DMA/code invalidation before executing the suffix. */
        tcg_gen_ld_i32(flags, tcg_constant_ptr(db->tb),
                       offsetof(TranslationBlock, cflags));
        tcg_gen_brcondi_i32(TCG_COND_TSTNE, flags, CF_INVALID, dc->compact_exit);
    }
    subleq_compact_body(db->pc_next, words, dc->ordered_read_pair);
    db->pc_next += SUBLEQ_INSN_SIZE;
    if (db->num_insns == dc->compact_len) {
        if (words[2] != db->pc_next && translator_use_goto_tb(db, words[2]) &&
            dc->direct_c_link) {
            subleq_gen_guarded_slot1(db, words[2]);
        }
        db->is_jmp = DISAS_TOO_MANY;
    }
}

static void subleq_tr_translate_insn(DisasContextBase *db, CPUState *cs)
{
    vaddr pc = db->pc_next;
    SubleqDisasContext *dc = container_of(db, SubleqDisasContext, base);
    if (db->num_insns == 1 && SUBLEQ_CPU(cs)->ram_burst) {
        dc->burst_len = subleq_plan_burst(dc, cs, false);
    } else if (db->num_insns == 1 && SUBLEQ_CPU(cs)->compact_tb) {
        dc->direct_c_link = SUBLEQ_CPU(cs)->direct_c_link;
#if defined(__x86_64__)
        dc->ordered_read_pair = SUBLEQ_CPU(cs)->ordered_read_pair &&
            !(tb_cflags(db->tb) & ~CF_CLUSTER_MASK) && !db->plugin_enabled;
#endif
        dc->compact_len = subleq_plan_burst(dc, cs, true);
    }
    if (dc->compact_len >= 2 || (dc->compact_len && pc >= SUBLEQ_RAM_SIZE)) {
        subleq_compact_region_insn(dc);
        return;
    }
    if (dc->burst_len >= 2) {
        subleq_gen_burst_insn(dc);
        return;
    }
    bool ram_insn = pc % 4 == 0 && pc <= SUBLEQ_RAM_SIZE - SUBLEQ_INSN_SIZE;
    bool retranslated = ram_insn && subleq_use_retranslation_helper(db, cs, pc);
    SubleqCPU *cpu = SUBLEQ_CPU(cs);
    bool predict_backedge = false;
    bool compact = false;

    /* Translation order supplies a hint, never an architectural target. */
    if (pc != cpu->translated_run_next ||
        !translator_is_same_page(db, cpu->translated_run_start)) {
        cpu->translated_run_start = pc;
    }
    cpu->translated_run_next = pc + SUBLEQ_INSN_SIZE;

    if (ram_insn && !retranslated) {
        TCGLabel *slow = gen_new_label();
        TCGLabel *done = gen_new_label();

        TCGv_i32 a, b, c;
        uint32_t words[3];
        uint32_t direct_target = 0;

        if (subleq_decode_ram_triple(db, cs, words)) {
            TCGv_ptr wp = tcg_temp_new_ptr();
            ptrdiff_t wp_offset = offsetof(CPUState, watchpoints.tqh_first);

            /*
             * Preserve helper-visible fetch reads even for watchpoints
             * installed after this TB was generated.
             */
            wp_offset -= (ptrdiff_t)offsetof(SubleqCPU, env);
            tcg_gen_ld_ptr(wp, tcg_env, wp_offset);
            tcg_gen_brcondi_ptr(TCG_COND_NE, wp, 0, slow);
            /* A common unconditional branch subtracts a word from itself. */
            predict_backedge = (words[2] & 1) && words[0] == words[1] &&
                !(words[0] & 1) && cpu->translated_run_start < pc &&
                !(tb_cflags(db->tb) & (CF_NOIRQ | CF_COUNT_MASK | CF_USE_ICOUNT)) &&
                translator_use_goto_tb(db, cpu->translated_run_start);
            if (cpu->direct_c_link && !(words[2] & 1) &&
                !(tb_cflags(db->tb) & ~CF_CLUSTER_MASK)) {
                uint32_t dest = words[2] & -4u;

                if (dest && dest != pc + SUBLEQ_INSN_SIZE &&
                    translator_use_goto_tb(db, dest)) {
                    direct_target = dest;
                }
            }
            /* Keep every instruction's tracked fetch/restore point, but move
             * the checked ordinary RAM operation into one shared C body. */
            compact = cpu->compact_tb && !(words[0] & 1) &&
                !(words[1] & 1) && !(words[2] & 1) &&
                (words[0] & -4u) != SUBLEQ_CLOCK &&
                (words[2] & -4u) != 0;
            if (!compact) {
                a = subleq_gen_decoded_operand(words[0], slow);
                b = subleq_gen_decoded_operand(words[1], slow);
                c = subleq_gen_decoded_operand(words[2], slow);
            }
        } else {
            a = subleq_gen_ram_operand(pc, slow);
            b = subleq_gen_ram_operand(pc + 4, slow);
            c = subleq_gen_ram_operand(pc + 8, slow);
        }
        if (compact) {
            subleq_compact_body(pc, words, dc->ordered_read_pair);
        } else {
            subleq_gen_ram_step(pc, a, b, c, slow, cpu->compact_tcg);
        }
        if (direct_target) {
            /* The full-step fallback (including watchpoints) skips this edge. */
            subleq_gen_guarded_slot1(db, direct_target);
        }
        tcg_gen_br(done);
        gen_set_label(slow);
        gen_helper_subleq_step(tcg_env);
        gen_set_label(done);
        if (predict_backedge) {
            subleq_gen_guarded_slot1(db, cpu->translated_run_start);
        }
    } else {
        gen_helper_subleq_step(tcg_env);
    }
    db->pc_next += SUBLEQ_INSN_SIZE;
    db->is_jmp = retranslated ? DISAS_RETRANSLATED : DISAS_TOO_MANY;
}

static void subleq_tr_tb_stop(DisasContextBase *db, CPUState *cs)
{
    SubleqDisasContext *dc = container_of(db, SubleqDisasContext, base);

    if (db->is_jmp == DISAS_BURST_DONE) {
        return;
    }
    if (db->is_jmp != DISAS_RETRANSLATED &&
        translator_use_goto_tb(db, db->pc_next)) {
        TCGLabel *other = gen_new_label();
        TCGv_i32 pc = tcg_temp_new_i32();

        /* Chain the sequential successor only if it matches the runtime PC. */
        tcg_gen_ld_i32(pc, tcg_env, offsetof(CPUSubleqState, pc));
        tcg_gen_brcondi_i32(TCG_COND_NE, pc, db->pc_next, other);
        tcg_gen_goto_tb(0);
        tcg_gen_exit_tb(db->tb, 0);
        gen_set_label(other);
    }
    tcg_gen_lookup_and_goto_ptr();
    if (dc->compact_exit) {
        gen_set_label(dc->compact_exit);
        tcg_gen_exit_tb(NULL, 0);
    }
}

static const TranslatorOps subleq_tr_ops = {
    .init_disas_context = subleq_tr_init_disas_context,
    .tb_start = subleq_tr_tb_start,
    .insn_start = subleq_tr_insn_start,
    .translate_insn = subleq_tr_translate_insn,
    .tb_stop = subleq_tr_tb_stop,
};

void subleq_translate_code(CPUState *cs, TranslationBlock *tb,
                           int *max_insns, vaddr pc, void *host_pc)
{
    SubleqDisasContext dc = { .burst_len = 0 };

    translator_loop(cs, tb, max_insns, pc, host_pc, &subleq_tr_ops, &dc.base);
}
