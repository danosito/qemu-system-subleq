#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

import struct
import subprocess
import time
from pathlib import Path

from qemu.machine import QEMUQtestProtocol
from qemu_test import QemuSystemTest


ROM_BASE = 0x7f000000
ROM_SIZE = 64 * 1024
MMIO_PROBE_READ = 0x7f010000
MMIO_PROBE_WRITE = 0x7f011000


class ESI(QemuSystemTest):
    def capsule(self, contents, name='capsule.bin'):
        size = max(addr + 4 * len(words) for addr, words in contents.items())
        image = bytearray(size)
        for addr, words in contents.items():
            struct.pack_into(f'<{len(words)}i', image, addr, *words)
        path = Path(self.scratch_file(name))
        path.write_bytes(image)
        return str(path)

    def run_board(self, *args, machine='esi'):
        return subprocess.run(
            [self.qemu_bin, '-M', machine, '-display', 'none',
             '-serial', 'stdio', '-monitor', 'none', *args],
            stdin=subprocess.DEVNULL, capture_output=True, timeout=15,
            check=False,
        )

    def run_capsule(self, contents):
        return self.run_board('-kernel', self.capsule(contents))

    def launch_stopped(self, *args, machine='esi'):
        self.set_machine('esi')
        self.vm.set_machine(machine)
        self.vm.add_args(*args, '-S', '-no-shutdown')
        self.vm.set_console()
        self.vm.launch()
        self.vm.console_socket.settimeout(10)

    def run_to_halt(self):
        self.vm.cmd('cont')
        event = self.vm.event_wait('SHUTDOWN', timeout=10)
        self.assertEqual(event['data']['reason'], 'guest-shutdown')
        self.vm.event_wait('STOP', timeout=10)

    def read_words(self, addr, count):
        output = self.vm.cmd('human-monitor-command',
                             command_line=f'xp /{count}wx {addr}')
        return [int(word, 16) for line in output.splitlines()
                for word in line.split(':', 1)[1].split()]

    def test_operands_and_wrapping_subtract(self):
        result = self.run_capsule({
            0: (0, 0, 12),
            # INT32_MIN - 1 wraps positive; fall through using indirect A/B.
            12: (0x301, 0x305, 36),
            # Resolve C through B before overwriting B. Zero branches.
            24: (0x308, 0x30c, 0x30d),
            36: (0x318, -4, 0),
            0x300: (0x310, 0x314, 0xffc, 0xffc, 1, -2**31, ord('F')),
            # A/B/C can straddle a target page boundary.
            0xffc: (0x1008, -4, 0, ord('K')),
        })
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, b'K')

    def test_self_modifying_code_and_qmp_reset(self):
        self.launch_stopped('-kernel', self.capsule({
            0: (0, 0, 12),
            12: (0x300, -4, 36),  # I/O falls through, ignoring nonzero C.
            24: (0x308, 12, 36),  # Change the already executed A operand.
            36: (0x30c, 0x310, 12),  # Branch to 12 only the first time.
            48: (0, 0, 0),
            0x300: (ord('A'), ord('B'), -4, -1, -1),
        }))
        self.assertEqual(self.vm.cmd('query-target')['arch'], 'subleq')
        self.assertEqual(self.vm.cmd('query-cpus-fast')[0]['target'], 'subleq')
        self.run_to_halt()
        console = self.vm.console_socket
        self.assertEqual(console.recv(1) + console.recv(1), b'AB')
        self.assertEqual(self.read_words(12, 1), [0x308])

        self.vm.cmd('system_reset')
        self.vm.event_wait('RESET', timeout=10)
        self.assertEqual(self.read_words(12, 1), [0x300])
        self.run_to_halt()
        self.assertEqual(console.recv(1) + console.recv(1), b'AB')

    def test_guest_write_invalidates_literal_operand(self):
        result = self.run_capsule({
            0: (0, 0, 12),
            12: (0x2004, 0x2010, 24),
            24: (0x2010, -4, 36),
            36: (0x2010, 0x2010, 48),
            48: (0x2000, 12, 60),  # Change A of the cached subtract at PC 12.
            60: (0x2014, 0x2018, 12),
            72: (0, 0, 0),
            0x2000: (-4, -ord('X'), -ord('K'), 0, 0, -1, -1),
        })
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, b'XK')

    def test_guest_write_retargets_indirect_operand(self):
        result = self.run_capsule({
            0: (0, 0, 12),
            12: (0x201d, 0x2010, 24),  # Load A through pointer at 0x201c.
            24: (0x2010, -4, 36),
            36: (0x2010, 0x2010, 48),
            48: (0x2000, 12, 60),  # Change pointer location to 0x2020.
            60: (0x2014, 0x2018, 12),
            72: (0, 0, 0),
            0x2000: (-4, -ord('X'), -ord('K'), 0, 0, -1, -1, 0x2004, 0x2008),
        })
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, b'XK')

    def test_physical_write_invalidates_literal_operand(self):
        image = self.capsule({
            0: (0, 0, 512),
            512: (0x2008, 0x2008, 524),
            524: (0x2000, 0x2008, 536),
            536: (0x2008, -4, 548),
            548: (0x2008, 0x2008, 560),
            560: (0x200c, 0x2010, 560),  # Wait for a nonzero gate word.
            572: (0x200c, 0x200c, 512),
            0x2000: (-ord('X'), -ord('K'), 0, 0, 0),
        })
        path = str(Path(self.socket_dir().name, 'qtest.sock'))
        self.launch_stopped('-kernel', image,
                            '-qtest', f'unix:{path},server=on,wait=off')
        self.vm.cmd('cont')
        self.assertEqual(self.vm.console_socket.recv(1), b'X')
        self.vm.cmd('stop')
        self.vm.event_wait('STOP', timeout=10)

        # Write through QEMU's physical address space with TCG still enabled.
        qtest = QEMUQtestProtocol(path)
        self.addCleanup(qtest.close)
        qtest.settimeout(10)
        qtest.connect()
        for address, value in ((524, 0x2004), (544, 0), (0x2010, 1)):
            response = qtest.cmd(f'writel {address:#x} {value:#x}')
            self.assertEqual(response.strip(), 'OK')
        self.run_to_halt()
        self.assertEqual(self.vm.console_socket.recv(1), b'K')

    def test_retranslation_helper_refetches_operand_after_reset(self):
        image = self.capsule({
            0: (0, 0, 12),
            12: (0x2004, 0x2020, 24),
            24: (0x2020, -4, 36),
            36: (0x2020, 0x2020, 48),
            48: (0x2000, 12, 60),  # Rewrite the same A word on every trip.
            60: (0x2018, 0x201c, 12),
            72: (0, 0, 0),
            0x2000: (-4, -ord('A'), -ord('B'), -ord('C'), -ord('D'),
                     -ord('E'), -1, -4, 0),
        })
        self.launch_stopped('-kernel', image, '-icount', 'shift=0,sleep=off',
                            '-global', 'subleq-cpu.x-retranslation-helper=on')
        console = self.vm.console_socket
        for run in range(2):
            if run:
                self.vm.cmd('system_reset')
                self.vm.event_wait('RESET', timeout=10)
            self.run_to_halt()
            self.assertEqual(b''.join(console.recv(1) for _ in range(5)),
                             b'ABCDE')

    def test_indirect_backedge_changes_after_link(self):
        result = self.run_capsule({
            0: (0, 0, 512),
            512: (0x2010, -4, 524),
            524: (0x2000, 0x2004, 560),
            536: (0x2014, 0x2014, 0x2009),  # Indirect C initially 512.
            560: (0x200c, 0x2008, 572),  # Change C without modifying code.
            572: (0x2014, 0x2014, 536),
            600: (0x2018, -4, 0),
            0x2000: (1, 3, 512, -88, ord('X'), 0, ord('K')),
        })
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, b'XXXK')

    def test_direct_c_link_refetches_patched_target(self):
        image = self.capsule({
            0: (0, 0, 512),
            512: (0x2010, 0x2010, 560),
            560: (0x2000, -4, 572),
            572: (0x2004, 0x2008, 596),
            584: (0x2010, 0x2010, 512),
            596: (0x200c, 520, 608),  # Retarget the warmed link to 624.
            608: (0x2010, 0x2010, 512),
            624: (0x2014, -4, 0),
            0x2000: (ord('X'), 1, 2, -64, 0, ord('K')),
        })
        result = self.run_board('-global', 'subleq-cpu.x-direct-c-link=on',
                                '-kernel', image)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, b'XXK')

    def test_direct_c_link_timer_overrides_taken_target(self):
        self.launch_stopped('-global', 'subleq-cpu.x-direct-c-link=on',
                            '-kernel', self.capsule({
            0: (0, 0, 512),
            512: (0x2000, 0, 524),  # Install the vector and tick once.
            524: (0x2004, 0x2008, 524),  # Always taken until expiry.
            536: (0, 0, 0),
            640: (0x200c, -4, 0),
            0x2000: (-640, 1, 0, ord('I')),
        }))
        self.run_to_halt()
        self.assertEqual(self.vm.console_socket.recv(1), b'I')
        self.assertEqual(self.read_words(4, 1), [524])
        self.assertEqual(self.read_words(0x2008, 1), [(-800001) & 0xffffffff])

    def test_direct_c_link_mmio_replay_once(self):
        last_ram_word = 0x5ffffffc
        self.launch_stopped('-global', 'subleq-cpu.x-direct-c-link=on',
                            '-kernel', self.capsule({
            0: (0, 0, 512),
            512: (0x2000, 0x2011, 560),  # Same taken C: first RAM, then MMIO.
            524: (0, 0, 0),
            560: (0x2004, 0x2010, 572),
            572: (0x2008, 0x2014, 512),
            584: (0x200c, -4, 0),
            0x2000: (7, last_ram_word - MMIO_PROBE_WRITE, -1, ord('M'),
                     last_ram_word, -1),
        }), machine='esi,x-mmio-probe=on')
        self.run_to_halt()
        self.assertEqual(self.vm.console_socket.recv(1), b'M')
        self.assertEqual(self.read_words(last_ram_word, 1), [0xfffffff9])
        self.assertEqual(self.read_words(MMIO_PROBE_READ, 2), [0xfffffff9, 1])

    def test_direct_c_link_unlinks_rewritten_destination(self):
        image = self.capsule({
            0: (0, 0, 512),
            512: (0x2014, 0x2014, 560),
            560: (0x2000, 0x2008, 572),
            572: (0x2008, -4, 584),
            584: (0x2008, 0x2008, 596),
            596: (0x200c, 0x2010, 620),
            608: (0x2014, 0x2014, 512),
            620: (0x2014, 0x2018, 620),  # Wait for the host to open the gate.
            632: (0x2014, 0x2014, 512),
            0x2000: (-ord('X'), -ord('K'), 0, 1, 2, 0, 0),
        })
        path = str(Path(self.socket_dir().name, 'qtest.sock'))
        self.launch_stopped('-global', 'subleq-cpu.x-direct-c-link=on',
                            '-kernel', image,
                            '-qtest', f'unix:{path},server=on,wait=off')
        self.vm.cmd('cont')
        console = self.vm.console_socket
        self.assertEqual(console.recv(1) + console.recv(1), b'XX')
        self.vm.cmd('stop')
        self.vm.event_wait('STOP', timeout=10)
        qtest = QEMUQtestProtocol(path)
        self.addCleanup(qtest.close)
        qtest.settimeout(10)
        qtest.connect()
        # Change only A of the linked destination, halt after output, resume.
        for addr, value in ((560, 0x2004), (580, 0), (0x2018, 1)):
            self.assertEqual(qtest.cmd(f'writel {addr:#x} {value:#x}').strip(),
                             'OK')
        self.run_to_halt()
        self.assertEqual(console.recv(1), b'K')

    def test_timer_and_clock(self):
        start = int(time.time())
        self.launch_stopped('-kernel', self.capsule({
            0: (0x1000, 0x300, 12),  # The A word also installs the IRQ handler.
            12: (0x304, 0x308, 12),  # A taken branch still advances the timer.
            24: (0, 0, 0),  # Fail by halting without firing the interrupt.
            0x300: (0, -1, -800001),
            0x1000: (0x100, 0x300, 0),  # Update the clock before halting.
        }))
        self.run_to_halt()
        # The first tick is at PC 0, then 800001 iterations of PC 12.
        self.assertEqual(self.read_words(4, 1), [12])
        self.assertEqual(self.read_words(0x308, 1), [0])
        low, high, nsec = self.read_words(0x100, 3)
        self.assertLess(nsec, 1_000_000_000)
        self.assertLessEqual(start, (high << 32) | low)
        self.assertLessEqual((high << 32) | low, int(time.time()))

    def test_burst_timer_expires_before_suffix(self):
        self.launch_stopped('-global', 'subleq-cpu.x-ram-burst=on',
                            '-kernel', self.capsule({
            0: (0, 0, 512),
            512: (0x2000, 0, 524),  # Install vector; first tick.
            524: (0x2004, 0x2008, 536),
            536: (0x200c, 0x200c, 524),
            1024: (0x2010, -4, 0),
            0x2000: (-1024, -1, 0, 0, ord('T')),
        }))
        self.run_to_halt()
        self.assertEqual(self.vm.console_socket.recv(1), b'T')
        self.assertEqual(self.read_words(4, 1), [536])
        self.assertEqual(self.read_words(0x2008, 1), [400001])

    def test_burst_exits_before_mmio_store(self):
        self.launch_stopped('-global', 'subleq-cpu.x-ram-burst=on',
                            '-kernel', self.capsule({
            0: (0, 0, 512),
            512: (0x2000, 0x2008, 524),
            524: (0x2000, 0x2008, 536),
            536: (0x2008, 0x2005, 548),  # Indirect B resolves to MMIO.
            548: (0x200c, -4, 0),
            0x2000: (1, MMIO_PROBE_WRITE, 0, ord('K')),
        }), machine='esi,x-mmio-probe=on')
        self.run_to_halt()
        self.assertEqual(self.vm.console_socket.recv(1), b'K')
        self.assertEqual(self.read_words(MMIO_PROBE_READ, 2), [2, 1])

    def test_burst_store_invalidates_suffix(self):
        image = self.capsule({
            0: (0, 0, 512),
            512: (0x2018, 0x2010, 524),
            524: (0x2004, 0x2010, 536),
            536: (0x2010, -4, 548),
            548: (0x2010, 0x2010, 560),
            560: (0x2000, 524, 572),  # Rewrite A of the second burst op.
            572: (0x200c, 0x2014, 512),
            584: (0, 0, 0),
            0x2000: (-4, -ord('X'), -ord('K'), -1, 0, -1, 0),
        })
        result = self.run_board('-global', 'subleq-cpu.x-ram-burst=on',
                                '-kernel', image)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, b'XK')

    def test_halt_status(self):
        result = self.run_capsule({0: (-4, 12, -4, 42)})
        self.assertEqual(result.returncode, 42, result.stderr)

    def test_out_of_range_operand(self):
        result = self.run_capsule({0: (0x60000001, -4, 0)})
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn(b'invalid address 0x60000000 at PC 0x00000000',
                      result.stderr)

    def firmware(self, marker, name='firmware.bin'):
        # ROM code increments a RAM word, prints its own marker, and halts.
        return self.capsule({
            0: (ROM_BASE + 24, 0x20, ROM_BASE + 12),
            12: (ROM_BASE + 28, -4, 0),
            24: (-1, ord(marker)),
        }, name)

    def test_firmware_reset(self):
        firmware = self.firmware('R')
        self.launch_stopped('-bios', firmware)
        console = self.vm.console_socket

        for count in (1, 2):
            if count == 2:
                self.vm.cmd('system_reset')
                self.vm.event_wait('RESET', timeout=10)
            regs = self.vm.cmd('human-monitor-command',
                               command_line='info registers')
            self.assertIn('PC=7f000000 TIMER=0', regs)
            self.assertEqual(self.read_words(ROM_BASE + 24, 2),
                             [0xffffffff, ord('R')])
            self.assertEqual(self.read_words(0x20, 1), [count - 1])
            self.run_to_halt()
            self.assertEqual(console.recv(1), b'R')
            self.assertEqual(self.read_words(0x20, 1), [count])

    def test_firmware_selection(self):
        firmware = self.firmware('S')
        result = self.run_board('-bios', firmware)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, b'S')

        result = self.run_board()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, b'')

        kernel = self.capsule({0: (0, 0, 0)})
        result = self.run_board('-bios', firmware, '-kernel', kernel)
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn(b'-bios and -kernel are mutually exclusive',
                      result.stderr)

    def test_firmware_size(self):
        absent = self.scratch_file('absent.bin')
        result = self.run_board('-bios', absent)
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn(b'Could not read ESI firmware', result.stderr)
        self.assertIn(b'absent.bin', result.stderr)

        firmware = Path(self.firmware('S'))
        for size in (ROM_SIZE, ROM_SIZE + 1, 11, 0):
            with self.subTest(size=size):
                with firmware.open('r+b') as image:
                    image.truncate(size)
                result = self.run_board('-bios', str(firmware))
                if size == ROM_SIZE:
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertEqual(result.stdout, b'S')
                else:
                    self.assertEqual(result.returncode, 1, result.stderr)
                    self.assertIn(b'must be 12 to 65536 bytes', result.stderr)

    def test_firmware_read_only(self):
        firmware = self.capsule({
            0: (ROM_BASE + 12, ROM_BASE + 12, 0, -1),
        }, 'firmware.bin')
        result = self.run_board('-bios', firmware)
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn(b'invalid address 0x7f00000c at PC 0x7f000000',
                      result.stderr)

    def test_mmio_write_alias(self):
        value = 0x89abcdef
        rom = self.capsule({
            # Read the alias and the count before the store.
            0: (MMIO_PROBE_WRITE, 0x200, ROM_BASE + 12),
            12: (MMIO_PROBE_READ + 4, 0x204, ROM_BASE + 24),
            # One Subleq instruction stores 0 - (-value) into the latch.
            24: (ROM_BASE + 96, MMIO_PROBE_WRITE, ROM_BASE + 36),
            # Neither the normal aperture nor the alias read may retrigger it.
            36: (MMIO_PROBE_WRITE, 0x200, ROM_BASE + 48),
            48: (MMIO_PROBE_READ, 0x20c, ROM_BASE + 60),
            60: (MMIO_PROBE_READ + 4, 0x210, ROM_BASE + 72),
            72: (ROM_BASE + 100, -4, 0),
            96: ((-value) & 0xffffffff, ord('W')),
        }, 'mmio-probe.bin')
        self.launch_stopped('-bios', rom, machine='esi,x-mmio-probe=on')
        self.assertEqual(self.read_words(MMIO_PROBE_READ, 2), [0, 0])
        self.run_to_halt()
        self.assertEqual(self.vm.console_socket.recv(1), b'W')
        self.assertEqual(self.read_words(0x200, 2), [0, 0])
        self.assertEqual(self.read_words(0x20c, 2),
                         [(-value) & 0xffffffff, 0xffffffff])
        self.assertEqual(self.read_words(MMIO_PROBE_READ, 2), [value, 1])
        self.assertEqual(self.read_words(MMIO_PROBE_WRITE, 1), [0])
        self.assertEqual(self.read_words(MMIO_PROBE_READ + 4, 1), [1])

        self.vm.cmd('system_reset')
        self.vm.event_wait('RESET', timeout=10)
        self.assertEqual(self.read_words(MMIO_PROBE_READ, 2), [0, 0])

    def test_ram_indirect_destination_changes_to_mmio_with_icount(self):
        value = 0x12345678
        last_ram_word = 0x5ffffffc
        image = self.capsule({
            0: (0, 0, 12),
            # The same instruction first writes RAM, then the MMIO alias.
            12: (0x3000, 0x3011, 24),
            24: (0x3004, 0x3010, 36),  # Change B's pointer, not the triple.
            36: (0x3008, 0x3014, 12),  # Loop back exactly once.
            48: (MMIO_PROBE_READ + 4, 0x3018, 60),
            60: (0x300c, -4, 0),
            0x3000: (-value, last_ram_word - MMIO_PROBE_WRITE, -1, ord('K'),
                     last_ram_word, -1, 0),
        })
        self.launch_stopped('-kernel', image, '-icount', 'shift=0,sleep=off',
                            machine='esi,x-mmio-probe=on')
        self.run_to_halt()
        self.assertEqual(self.vm.console_socket.recv(1), b'K')
        self.assertEqual(self.read_words(last_ram_word, 1), [value])
        self.assertEqual(self.read_words(0x3018, 1), [0xffffffff])
        self.assertEqual(self.read_words(MMIO_PROBE_READ, 2), [value, 1])

    def test_mmio_probe_access_policy(self):
        for machine, address in (('esi', MMIO_PROBE_WRITE),
                                 ('esi,x-mmio-probe=on', MMIO_PROBE_READ)):
            with self.subTest(machine=machine, address=address):
                image = self.capsule({0: (12, address, 0, -1)})
                result = self.run_board('-kernel', image, machine=machine)
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertIn(f'invalid address 0x{address:08x}'.encode(),
                              result.stderr)


if __name__ == '__main__':
    QemuSystemTest.main()
