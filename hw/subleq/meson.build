# SPDX-License-Identifier: GPL-2.0-or-later

subleq_ss = ss.source_set()
subleq_ss.add(when: 'CONFIG_ESI', if_true: files('esi.c'))
hw_arch += {'subleq': subleq_ss}
