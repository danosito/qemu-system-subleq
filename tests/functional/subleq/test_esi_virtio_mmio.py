#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

import ipaddress
import socket
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

from qemu.machine.qtest import QEMUQtestMachine
from qemu_test import QemuSystemTest


READ = 0x60000000
WRITE = 0x61000000
PENDING = 0x62000000
ROM = 0x7f000000
PROBE_READ = 0x7f010000
PROBE_WRITE = 0x7f011000
RAM_END = 0x60000000  # Exclusive.
STRIDE = 0x1000
MACHINE = 'esi,x-virtio-mmio=on'

MAGIC, VERSION, DEVICE_ID, VENDOR = 0x00, 0x04, 0x08, 0x0c
DEVICE_FEATURES, DEVICE_FEATURES_SEL = 0x10, 0x14
DRIVER_FEATURES, DRIVER_FEATURES_SEL = 0x20, 0x24
QUEUE_SEL, QUEUE_NUM_MAX, QUEUE_NUM = 0x30, 0x34, 0x38
QUEUE_READY, QUEUE_NOTIFY = 0x44, 0x50
INTERRUPT_STATUS, INTERRUPT_ACK, STATUS = 0x60, 0x64, 0x70
QUEUE_DESC, QUEUE_AVAIL, QUEUE_USED = 0x80, 0x90, 0xa0
CONFIG = 0x100
ACKNOWLEDGE, DRIVER, DRIVER_OK, FEATURES_OK = 1, 2, 4, 8
VRING_INTERRUPT = 1
DESC_NEXT, DESC_WRITE = 1, 2
BLK_IN, BLK_OUT, BLK_FLUSH = 0, 1, 4
BLK_F_FLUSH = 1 << 9


class Capsule:
    """Raw ESI instructions and preloaded RAM for a minimal polled driver."""

    def __init__(self):
        self.image = bytearray(0x10000)
        self.pc = 0
        self.next_word = 0x2000
        self.op(0, 0)  # Leave the timer interrupt vector zero.

    def ram(self, addr, data):
        self.image[addr:addr + len(data)] = data

    def word(self, value):
        addr = self.next_word
        self.next_word += 4
        self.ram(addr, struct.pack('<I', value & 0xffffffff))
        return addr

    def op(self, a, b, branch=None):
        dest = self.pc + 12 if branch is None else branch
        self.ram(self.pc, struct.pack('<III', a, b, dest))
        self.pc += 12

    def add(self, addr, value):
        self.op(self.word(-value), addr)

    def wr(self, slot, reg, value):
        # The write alias's B load is zero.
        self.add(WRITE + slot * STRIDE + reg, value)

    def read(self, addr):
        neg, result = self.word(0), self.word(0)
        self.op(addr, neg)
        self.op(neg, result)
        return result

    def wait_used(self, used, count=1):
        # The used word also contains 16 bits of flags; ignore those bits.
        neg, result = self.word(0), self.word(0)
        threshold = self.word((count << 16) - 1)
        loop = self.pc
        self.op(neg, neg)
        self.op(used, neg)
        self.op(result, result)
        self.op(threshold, result)
        self.op(neg, result, loop)

    def rd(self, slot, reg):
        return self.read(READ + slot * STRIDE + reg)

    def negotiate(self, slot, low_features=0):
        self.wr(slot, STATUS, 0)
        state = ACKNOWLEDGE | DRIVER
        self.wr(slot, STATUS, ACKNOWLEDGE)
        self.wr(slot, STATUS, state)
        self.wr(slot, DEVICE_FEATURES_SEL, 1)
        version_feature = self.rd(slot, DEVICE_FEATURES)
        self.wr(slot, DRIVER_FEATURES_SEL, 0)
        self.wr(slot, DRIVER_FEATURES, low_features)
        self.wr(slot, DRIVER_FEATURES_SEL, 1)
        self.wr(slot, DRIVER_FEATURES, 1)  # VIRTIO_F_VERSION_1
        self.wr(slot, STATUS, state | FEATURES_OK)
        return version_feature, self.rd(slot, STATUS)

    def queue(self, slot, index, desc, avail, used):
        self.wr(slot, QUEUE_SEL, index)
        maximum = self.rd(slot, QUEUE_NUM_MAX)
        self.wr(slot, QUEUE_NUM, 8)
        for reg, base in ((QUEUE_DESC, desc), (QUEUE_AVAIL, avail),
                          (QUEUE_USED, used)):
            self.wr(slot, reg, base)
            self.wr(slot, reg + 4, 0)
        self.wr(slot, QUEUE_READY, 1)
        return maximum, self.rd(slot, QUEUE_READY)

    def start(self, slot):
        self.wr(slot, STATUS, ACKNOWLEDGE | DRIVER | FEATURES_OK | DRIVER_OK)

    def ack(self, slot):
        pending = self.rd(slot, INTERRUPT_STATUS)
        self.wr(slot, INTERRUPT_ACK, VRING_INTERRUPT)
        return pending, self.rd(slot, INTERRUPT_STATUS)

    def descriptor(self, table, index, addr, length, flags=0, next_idx=0):
        self.ram(table + 16 * index,
                 struct.pack('<QIHH', addr, length, flags, next_idx))


class ESIVirtioMMIO(QemuSystemTest):
    def image(self, program):
        path = Path(self.scratch_file('virtio.capsule'))
        path.write_bytes(program.image)
        return str(path)

    def launch_capsule(self, program, *args):
        program.op(0, 0, 0)
        vm = self.get_vm('-kernel', self.image(program), '-S', '-no-shutdown',
                         *args)
        vm.set_machine(MACHINE)
        vm.launch()
        vm.cmd('cont')
        event = vm.event_wait('SHUTDOWN', timeout=10)
        self.assertEqual(event['data']['reason'], 'guest-shutdown')
        vm.event_wait('STOP', timeout=10)
        return vm

    def read_ram(self, vm, addr, length):
        path = Path(self.scratch_file('virtio.ram'))
        vm.cmd('pmemsave', val=addr, size=length, filename=str(path))
        return path.read_bytes()

    def word(self, vm, addr):
        return struct.unpack('<I', self.read_ram(vm, addr, 4))[0]

    def assert_words(self, vm, addrs, values):
        self.assertEqual([self.word(vm, addr) for addr in addrs], values)

    def test_guest_mmio_checks(self):
        cases = (('esi', READ, 0, None, READ),
                 ('esi', PENDING, 0, None, PENDING),
                 (MACHINE, 0, READ, None, READ),
                 (MACHINE, 0, PENDING, None, PENDING),
                 (MACHINE, PENDING + 4, 0, None, PENDING + 4),
                 (MACHINE, READ + 3 * STRIDE, 0, None, READ + 3 * STRIDE),
                 (MACHINE, 0, WRITE + 3 * STRIDE, None, WRITE + 3 * STRIDE),
                 (MACHINE, WRITE + 1, 0, None, WRITE),  # Indirect load.
                 (MACHINE, READ, 0xfffffffc, None, READ),  # Console load.
                 (MACHINE, 0, 0, READ, READ))  # Instruction fetch.
        for machine, a, b, branch, bad in cases:
            with self.subTest(machine=machine, a=a, b=b, branch=branch):
                program = Capsule()
                program.op(a, b, branch)
                program.op(0, 0, 0)
                result = subprocess.run(
                    [self.qemu_bin, '-M', machine, '-display', 'none',
                     '-serial', 'null', '-monitor', 'none', '-kernel',
                     self.image(program)], capture_output=True, timeout=10,
                    check=False)
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertIn(f'invalid address 0x{bad:08x}'.encode(),
                              result.stderr)

    def test_guest_mmio_identity_alias_and_reset(self):
        disk = Path(self.scratch_file('identity.raw'))
        disk.write_bytes(bytes(4096))
        program = Capsule()
        registers = [program.rd(0, reg) for reg in
                     (MAGIC, VERSION, DEVICE_ID, VENDOR)]
        empty = [program.rd(slot, DEVICE_ID) for slot in (1, 2)]
        program.wr(0, STATUS, ACKNOWLEDGE)
        program.wr(0, STATUS, ACKNOWLEDGE | DRIVER)
        program.wr(0, STATUS, ACKNOWLEDGE | DRIVER)
        status = program.rd(0, STATUS)
        alias = program.read(WRITE + STATUS)
        padding = program.rd(2, STRIDE - 4)
        vm = self.launch_capsule(program, '-drive',
                                 f'if=none,id=disk,format=raw,file={disk}',
                                 '-device', 'virtio-blk-device,drive=disk,'
                                 'bus=virtio-mmio-bus.0')
        self.assert_words(vm, registers, [0x74726976, 2, 2, 0x554d4551])
        self.assert_words(vm, empty, [0, 0])
        self.assert_words(vm, [status, alias, padding], [3, 0, 0])
        vm.cmd('system_reset')
        vm.event_wait('RESET', timeout=10)
        self.assertEqual(self.word(vm, READ + STATUS), 0)

    def test_bus_alias_and_access_width(self):
        disk = Path(self.scratch_file('access.raw'))
        disk.write_bytes(bytes(4096))
        args = ('-M', MACHINE, '-drive',
                f'if=none,id=disk,format=raw,file={disk}',
                '-device', 'virtio-blk-device,drive=disk,'
                'bus=virtio-mmio-bus.0')
        with QEMUQtestMachine(self.qemu_bin, args,
                              base_temp_dir=self.workdir) as vm:
            vm.launch()

            def readl(addr):
                status, value = vm.qtest(f'readl {addr:#x}').split()
                self.assertEqual(status, 'OK')
                return int(value, 16)

            self.assertEqual(readl(READ + MAGIC), 0x74726976)
            self.assertEqual(readl(WRITE + MAGIC), 0)
            self.assertEqual(vm.qtest(f'writel {WRITE + STATUS:#x} 1').strip(),
                             'OK')
            self.assertEqual(readl(READ + STATUS), 1)
            # qtest returns OK even if the bus declines the transaction.
            for cmd, addr in (('writel', READ + STATUS),
                              ('writeb', WRITE + STATUS),
                              ('writel', WRITE + STATUS + 1)):
                vm.qtest(f'{cmd} {addr:#x} 0')
                self.assertEqual(readl(READ + STATUS), 1)
            self.assertEqual(readl(WRITE + STATUS), 0)

    def test_dma_address_space_boundaries_and_reset(self):
        bios = Path(self.scratch_file('dma-bios.raw'))
        bios.write_bytes(struct.pack('<III', 4, 0, 0))
        disk = Path(self.scratch_file('dma-test.raw'))
        disk.write_bytes(bytes(4096))
        args = ('-M', MACHINE + ',x-mmio-probe=on', '-bios', str(bios),
                '-drive', f'if=none,id=disk,format=raw,file={disk}',
                '-device', 'virtio-blk-device,drive=disk,'
                'bus=virtio-mmio-bus.0')
        with QEMUQtestMachine(self.qemu_bin, args,
                              base_temp_dir=self.workdir) as vm:
            vm.launch()

            # ROM and transport MMIO really exist in the system view.
            for addr, value in ((ROM, 4), (READ + MAGIC, 0x74726976)):
                status, result = vm.qtest(f'readl {addr:#x}').split()
                self.assertEqual(status, 'OK')
                self.assertEqual(int(result, 16), value)

            for reset in (False, True):
                if reset:
                    vm.cmd('system_reset')
                    vm.event_wait('RESET', timeout=10)
                for addr, length, valid in (
                    (0, 16, True), (RAM_END - 4, 4, True),
                    (RAM_END - 1, 1, True), (RAM_END - 1, 2, False),
                    (RAM_END, 1, False), (READ, 4, False),
                    (WRITE, 4, False), (PENDING, 4, False),
                    (ROM, 4, False), (PROBE_READ, 4, False),
                    (PROBE_WRITE, 4, False), (0x100000000, 4, False),
                ):
                    with self.subTest(reset=reset, addr=addr, length=length):
                        result = vm.qtest(f'esi-virtio-dma-valid 0 {addr:#x} '
                                          f'{length}').split()
                        self.assertEqual(result, ['OK', str(int(valid)),
                                                  str(int(valid))])

    def assert_plug_rejected(self, device, error):
        result = subprocess.run(
            [self.qemu_bin, '-M', MACHINE, '-display', 'none', '-serial',
             'null', '-nodefaults', '-S', '-device',
             device + ',bus=virtio-mmio-bus.0'], capture_output=True,
            timeout=10, check=False)
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn(error, result.stderr)

    def test_dma_rejects_unaudited_frontend(self):
        self.require_device('virtio-balloon-device')
        self.assert_plug_rejected('virtio-balloon-device',
                                  b'ESI virtio-mmio supports only')

    def test_rng_dma_view(self):
        if sys.platform != 'linux':
            self.skipTest('requires Linux urandom')
        args = ('-M', MACHINE, '-object', 'rng-random,id=r,filename=/dev/urandom',
                '-device', 'virtio-rng-device,rng=r,max-bytes=64,'
                'bus=virtio-mmio-bus.2')
        with QEMUQtestMachine(self.qemu_bin, args,
                              base_temp_dir=self.workdir) as vm:
            vm.launch()
            for reset in (False, True):
                if reset:
                    vm.cmd('system_reset')
                    vm.event_wait('RESET', timeout=10)
                for addr, size, valid in ((0, 4, '1'), (RAM_END - 1, 1, '1'),
                                          (RAM_END - 1, 2, '0'), (READ, 4, '0'),
                                          (WRITE, 4, '0'), (PENDING, 4, '0'),
                                          (ROM, 4, '0'), (1 << 32, 1, '0')):
                    reply = vm.qtest(f'esi-virtio-dma-valid 2 {addr:#x} {size}')
                    self.assertEqual(reply.split(), ['OK', valid, valid])

    def test_rng_rejects_wrong_slot(self):
        self.assert_plug_rejected('virtio-rng-device', b'requires slot 2')

    def test_rng_rejects_unapproved_sources(self):
        if sys.platform != 'linux':
            self.skipTest('Linux source checks')
        device = 'virtio-rng-device,bus=virtio-mmio-bus.2'
        for obj, props, error in (
            (None, '', 'requires explicit rng-random'),
            ('rng-builtin,id=r', ',rng=r', 'requires explicit rng-random'),
            ('rng-random,id=r,filename=/dev/zero', ',rng=r',
             'requires ready Linux urandom'),
            ('rng-random,id=r,filename=/dev/urandom', ',rng=r',
             'requires max-bytes'),
            ('rng-random,id=r,filename=/dev/urandom',
             ',rng=r,max-bytes=64,iommu_platform=on',
             'does not support iommu_platform=on'),
        ):
            with self.subTest(obj=obj, props=props):
                cmd = [self.qemu_bin, '-M', MACHINE, '-display', 'none',
                       '-nodefaults', '-S']
                if obj:
                    cmd += ['-object', obj]
                result = subprocess.run(cmd + ['-device', device + props],
                                        capture_output=True, timeout=10)
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertIn(error.encode(), result.stderr)

    def test_rng_queue_irq_ack_and_reset(self):
        if sys.platform != 'linux':
            self.skipTest('requires Linux urandom')
        desc, avail, used = 0x4000, 0x5000, 0x6000
        backend = 'rng-random,id=r,filename=/dev/urandom'
        for quota in (64, 1):
            with self.subTest(quota=quota):
                prog = Capsule()
                device = prog.rd(2, DEVICE_ID)
                version, features = prog.negotiate(2)
                maximum, ready = prog.queue(2, 0, desc, avail, used)
                prog.descriptor(desc, 0, 0x8000, 32, DESC_WRITE)
                prog.descriptor(desc, 1, 0x8100, 32, DESC_WRITE)
                prog.ram(avail, struct.pack('<HHHH', 0, 0, 0, 1))
                prog.start(2)
                prog.add(avail, 1 << 16)
                prog.wr(2, QUEUE_NOTIFY, 0)
                prog.wait_used(used)
                asserted = prog.read(PENDING)
                isr, acked = prog.ack(2)
                clear = prog.read(PENDING)
                prog.add(avail, 1 << 16)
                prog.wr(2, QUEUE_NOTIFY, 0)
                if quota == 64:
                    prog.wait_used(used, 2)
                pending = prog.read(PENDING)
                vm = self.launch_capsule(
                    prog, '-object', backend, '-device',
                    'virtio-rng-device,rng=r,bus=virtio-mmio-bus.2,'
                    f'max-bytes={quota},period=60000')
                self.assertGreaterEqual(self.word(vm, maximum), 8)
                self.assert_words(vm, [device, features, ready,
                                       asserted, isr, acked, clear],
                                  [4, 11, 1, 4, 1, 0, 0])
                self.assertEqual(self.word(vm, version) & 3, 1)
                # Inspect used metadata only, never the receive buffers.
                self.assert_words(vm, [used + 4, used + 8], [0, min(quota, 32)])
                if quota == 64:
                    self.assert_words(vm, [used, used + 12, used + 16, pending],
                                      [2 << 16, 1, 32, 4])
                else:
                    self.assert_words(vm, [used, pending], [1 << 16, 0])
                vm.cmd('system_reset')
                vm.event_wait('RESET', timeout=10)
                self.assert_words(vm, [PENDING, READ + 2 * STRIDE + STATUS],
                                  [0, 0])
                vm.shutdown()

    def test_dma_rejects_iommu_platform(self):
        self.assert_plug_rejected('virtio-net-device,iommu_platform=on',
                                  b'does not support iommu_platform=on')

    def test_dma_rejects_vhost_user_net(self):
        self.require_netdev('vhost-user')
        # Stand in for a local backend through initial negotiation only.
        # There are no guest queues, no guest I/O and no network connections.
        get_features, set_owner = 1, 3
        set_vring_call, set_vring_err = 13, 14
        get_protocol_features, set_protocol_features = 15, 16
        vhost_user_version, reply = 1, 4
        header = struct.Struct('=III')
        response = struct.Struct('=IIIQ')
        features = (1 << 30) | (1 << 32)  # PROTOCOL_FEATURES, VERSION_1.
        replies = {get_features: features, get_protocol_features: 0}
        one_way = (set_owner, set_vring_call, set_vring_err,
                   set_protocol_features)
        with tempfile.TemporaryDirectory(prefix='vu-') as tmpdir, \
             socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as listener:
            path = str(Path(tmpdir) / 'vu.sock')
            listener.bind(path)
            listener.listen(1)
            listener.settimeout(10)
            cmd = [self.qemu_bin, '-M', MACHINE, '-display', 'none',
                   '-serial', 'null', '-nodefaults', '-S', '-chardev',
                   f'socket,id=vu,path={path}', '-netdev',
                   'vhost-user,id=n0,chardev=vu', '-device',
                   'virtio-net-device,netdev=n0,bus=virtio-mmio-bus.0']
            with subprocess.Popen(cmd, stderr=subprocess.PIPE) as proc:
                try:
                    peer, _ = listener.accept()
                    with peer, peer.makefile('rb') as stream:
                        peer.settimeout(10)
                        while msg := stream.read(header.size):
                            request, flags, size = header.unpack(msg)
                            self.assertEqual(flags, vhost_user_version)
                            self.assertIn(size, (0, 8))
                            self.assertEqual(len(stream.read(size)), size)
                            if request in replies:
                                value = replies[request]
                                peer.sendall(response.pack(
                                    request, flags | reply, 8, value))
                            else:
                                # No guest memory table is sent.
                                self.assertIn(request, one_way)
                    _, stderr = proc.communicate(timeout=10)
                finally:
                    if proc.poll() is None:
                        proc.kill()
                self.assertEqual(proc.returncode, 1, stderr)
                self.assertIn(b'does not support vhost network backends',
                              stderr)

    def test_guest_block_dma_flush_and_reopen(self):
        disk = Path(self.scratch_file('persist.raw'))
        disk.write_bytes(bytes(4096))
        marker = bytes(range(256)) * 2
        desc, avail, used = 0x4000, 0x4100, 0x4200
        header, data, status = 0x4300, 0x4400, 0x4600
        flush_header, flush_status = 0x4610, 0x4620

        for write in (True, False):
            program = Capsule()
            request = BLK_OUT if write else BLK_IN
            program.ram(header, struct.pack('<IIQ', request, 0, 1))
            program.ram(status, b'\xff')
            program.descriptor(desc, 0, header, 16, DESC_NEXT, 1)
            program.descriptor(desc, 1, data, len(marker),
                               DESC_NEXT | (0 if write else DESC_WRITE), 2)
            program.descriptor(desc, 2, status, 1, DESC_WRITE)
            program.ram(avail, struct.pack('<HHHH', 0, 1, 0, 3))
            if write:
                program.ram(data, marker)
                program.ram(flush_header, struct.pack('<IIQ', BLK_FLUSH, 0, 0))
                program.ram(flush_status, b'\xff')
                program.descriptor(desc, 3, flush_header, 16, DESC_NEXT, 4)
                program.descriptor(desc, 4, flush_status, 1, DESC_WRITE)

            version_feature, features_ok = program.negotiate(0, BLK_F_FLUSH)
            maximum, ready = program.queue(0, 0, desc, avail, used)
            program.start(0)
            program.wr(0, QUEUE_NOTIFY, 0)
            program.wait_used(used)
            acknowledgements = list(program.ack(0))
            if write:
                program.add(avail, 1 << 16)  # Submit flush (avail.idx = 2).
                program.wr(0, QUEUE_NOTIFY, 0)
                program.wait_used(used, count=2)
                acknowledgements.extend(program.ack(0))

            vm = self.launch_capsule(program, '-drive',
                                     f'if=none,id=disk,format=raw,file={disk}',
                                     '-device', 'virtio-blk-device,drive=disk,'
                                     'bus=virtio-mmio-bus.0')
            self.assertEqual(self.word(vm, version_feature) & 0b11, 1)
            self.assertGreaterEqual(self.word(vm, maximum), 8)
            self.assert_words(vm, [features_ok, ready], [11, 1])
            expected_acks = [1, 0] * (2 if write else 1)
            self.assert_words(vm, acknowledgements, expected_acks)
            self.assertEqual(self.read_ram(vm, status, 1), b'\0')
            used_idx, used_id, used_len = struct.unpack(
                '<HII', self.read_ram(vm, used + 2, 10))
            self.assertEqual((used_idx, used_id, used_len),
                             (2, 0, 1) if write else (1, 0, 513))
            if write:
                self.assertEqual(self.read_ram(vm, flush_status, 1), b'\0')
                self.assert_words(vm, [used + 12, used + 16], [3, 1])
            else:
                self.assertEqual(self.read_ram(vm, data, len(marker)), marker)
            vm.shutdown()
            self.assertEqual(disk.read_bytes()[512:1024], marker)

    def test_pending_bitmap_independent_slot_ack_and_system_reset(self):
        program = Capsule()
        initial = program.read(PENDING)
        disks = []
        queues = {}
        for slot in (0, 2):
            # Two real completion sources, without any network dependencies.
            disk = Path(self.scratch_file(f'irq{slot}.raw'))
            disk.write_bytes(bytes(4096))
            disks.extend(['-drive',
                          f'if=none,id=d{slot},format=raw,file={disk}',
                          '-device', f'virtio-blk-device,drive=d{slot},'
                          f'bus=virtio-mmio-bus.{slot}'])
            base = 0x4000 + 0x1000 * slot
            desc, avail, used = base, base + 0x100, base + 0x200
            header, data, status = base + 0x300, base + 0x400, base + 0x600
            program.ram(header, struct.pack('<IIQ', BLK_IN, 0, 0))
            program.ram(status, b'\xff')
            program.descriptor(desc, 0, header, 16, DESC_NEXT, 1)
            program.descriptor(desc, 1, data, 512, DESC_NEXT | DESC_WRITE, 2)
            program.descriptor(desc, 2, status, 1, DESC_WRITE)
            program.ram(avail, struct.pack('<HHHH', 0, 1, 0, 0))
            program.negotiate(slot)
            program.queue(slot, 0, desc, avail, used)
            program.start(slot)
            queues[slot] = avail, used, status

        for slot in queues:
            program.wr(slot, QUEUE_NOTIFY, 0)
        for _, used, _ in queues.values():
            program.wait_used(used)
        both = program.read(PENDING)
        first = program.ack(0)
        second_only = program.read(PENDING)
        second = program.ack(2)
        none = program.read(PENDING)

        for slot, (avail, _, _) in queues.items():
            program.add(avail, 1 << 16)
            program.wr(slot, QUEUE_NOTIFY, 0)
        for _, used, _ in queues.values():
            program.wait_used(used, count=2)
        reasserted = program.read(PENDING)

        vm = self.launch_capsule(program, *disks)
        self.assert_words(vm, [initial, both, second_only, none, reasserted],
                          [0, 0b101, 0b100, 0, 0b101])
        self.assert_words(vm, first + second, [1, 0, 1, 0])
        for _, used, status in queues.values():
            self.assertEqual(self.word(vm, used) >> 16, 2)
            self.assertEqual(self.read_ram(vm, status, 1), b'\0')
        self.assertEqual(self.word(vm, 4), 0)  # No asynchronous saved-PC write.
        vm.cmd('system_reset')
        vm.event_wait('RESET', timeout=10)
        self.assert_words(vm, [PENDING, READ + INTERRUPT_STATUS,
                               READ + 2 * STRIDE + INTERRUPT_STATUS], [0, 0, 0])

    def test_guest_net_dma_arp_and_second_nic_identity(self):
        self.require_netdev('user')
        program = Capsule()
        rx_desc, rx_avail, rx_used, rx_data = 0x4000, 0x4100, 0x4200, 0x4400
        tx_desc, tx_avail, tx_used, tx_data = 0x5000, 0x5100, 0x5200, 0x5400
        mac = bytes.fromhex('525400123456')
        other_mac = bytes.fromhex('525400123457')
        guest_ip = ipaddress.IPv4Address('10.0.2.15').packed
        router_ip = ipaddress.IPv4Address('10.0.2.2').packed
        # Modern 12-byte virtio header followed by a local ARP request.
        tx = (bytes(12) + b'\xff' * 6 + mac + b'\x08\x06' +
              b'\x00\x01\x08\x00\x06\x04\x00\x01' +
              mac + guest_ip + bytes(6) + router_ip)
        program.ram(tx_data, tx)
        program.descriptor(tx_desc, 0, tx_data, len(tx))
        program.descriptor(rx_desc, 0, rx_data, 2048, DESC_WRITE)
        for avail in (rx_avail, tx_avail):
            program.ram(avail, struct.pack('<HHH', 0, 1, 0))

        ids = [program.rd(slot, DEVICE_ID) for slot in (0, 1, 2)]
        mac_words = [[program.rd(slot, CONFIG + offset) for offset in (0, 4)]
                     for slot in (1, 2)]
        version_feature, features_ok = program.negotiate(1)
        rx_max, rx_ready = program.queue(1, 0, rx_desc, rx_avail, rx_used)
        tx_max, tx_ready = program.queue(1, 1, tx_desc, tx_avail, tx_used)
        program.start(1)
        program.wr(1, QUEUE_NOTIFY, 0)
        program.wr(1, QUEUE_NOTIFY, 1)
        program.wait_used(rx_used)
        program.wait_used(tx_used)
        acknowledgements = program.ack(1)
        vm = self.launch_capsule(
            program, '-netdev', 'user,id=n0,restrict=on,ipv6=off',
            '-device', 'virtio-net-device,netdev=n0,bus=virtio-mmio-bus.1,'
            'mac=52:54:00:12:34:56',
            '-netdev', 'user,id=n1,restrict=on,ipv6=off,net=10.0.3.0/24',
            '-device', 'virtio-net-device,netdev=n1,bus=virtio-mmio-bus.2,'
            'mac=52:54:00:12:34:57')
        self.assert_words(vm, ids, [0, 1, 1])
        for words, expected in zip(mac_words, (mac, other_mac)):
            config = struct.pack('<II', *(self.word(vm, a) for a in words))
            self.assertEqual(config[:6], expected)
        self.assertTrue(self.word(vm, version_feature) & 1)
        self.assertGreaterEqual(self.word(vm, rx_max), 8)
        self.assertGreaterEqual(self.word(vm, tx_max), 8)
        self.assert_words(vm, [features_ok, rx_ready, tx_ready], [11, 1, 1])
        self.assert_words(vm, acknowledgements, [1, 0])
        self.assertEqual(self.word(vm, rx_used) >> 16, 1)
        self.assertEqual(self.word(vm, tx_used) >> 16, 1)
        self.assert_words(vm, [rx_used + 4, tx_used + 4, tx_used + 8],
                          [0, 0, 0])
        self.assertGreaterEqual(self.word(vm, rx_used + 8), 12 + 42)
        reply = self.read_ram(vm, rx_data + 12, 42)
        self.assertEqual(reply[:6], mac)
        self.assertEqual(reply[12:22], bytes.fromhex('08060001080006040002'))
        self.assertEqual(reply[22:28], reply[6:12])
        self.assertEqual(reply[28:], router_ip + mac + guest_ip)


if __name__ == '__main__':
    QemuSystemTest.main()
