/* SPDX-License-Identifier: GPL-2.0-or-later */
DEF_HELPER_1(subleq_step, void, env)
DEF_HELPER_3(subleq_timer_irq, i32, env, i32, i32)
DEF_HELPER_3(subleq_burst_tick, void, env, i32, i32)
DEF_HELPER_FLAGS_1(subleq_burst_live, TCG_CALL_NO_WG, i32, ptr)
