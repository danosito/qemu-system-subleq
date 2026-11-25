.. SPDX-License-Identifier: GPL-2.0-or-later

Subleq System Emulator
----------------------

The ``qemu-system-subleq`` executable emulates a single-CPU Eternal
Software Initiative (ESI) machine, ``esi``, using TCG. It has 1536 MiB
of little-endian RAM starting at byte address zero. Pass an unmodified
raw serial ESI capsule with ``-kernel``; the machine loads it and starts
at byte address zero. For example:

.. code-block:: console

   $ qemu-system-subleq -M esi -kernel vmlinux.serial.bootimage \
       -display none -serial stdio -monitor none

Firmware can instead be selected with ``-bios path/to/firmware.bin``.
The image is a raw little-endian binary of 12 to 65536 bytes, loaded
into a read-only 64 KiB aperture at ``0x7f000000``. The CPU starts at
that address, including on system reset. The rest of the aperture is
zero-filled. The ROM is readable as data; an attempted guest write is
an address fault. It does not overlap RAM.

``-bios`` and ``-kernel`` are mutually exclusive. With neither option,
no firmware is loaded: the machine starts at zero in zero-filled RAM
and halts. There is no bundled firmware. The functional test at
``tests/functional/subleq/test_esi.py`` generates and runs two small,
open firmware examples; neither loads a disk.

The console uses the first QEMU serial chardev. A GETCHAR instruction
polls for a byte without blocking and leaves memory unchanged if none
is available. PUTCHAR sends the low byte of the source word. Both
continue to the next instruction unless the resolved C operand is zero
(the reference VM's legacy shutdown convention). The explicit
``-4, address, -4`` encoding shuts down with the low byte of the word at
``address`` as exit status.

The instruction counter follows the reference console VM: it advances
on every subtraction, including taken branches, when the interrupt
vector at address zero is nonzero. On interrupt, the next PC is stored
at byte address four. A subtraction whose resolved A operand is
``0x100`` refreshes the host wall clock there.

QMP can inspect, stop, resume, or reset the VM. System reset resets the
PC, instruction counter, and console input queue. It restores the
capsule bytes in direct boot mode; RAM outside the capsule is retained.
In firmware mode RAM is retained and the ROM stays mapped and unchanged.
A future firmware-to-disk handoff must use a nonzero entry point: a
resolved C value of zero still halts in both boot modes.

Experimental MMIO probe
~~~~~~~~~~~~~~~~~~~~~~~

``-M esi,x-mmio-probe=on`` enables a test-only pair of aligned,
32-bit little-endian MMIO windows. The default is off; the addresses
and interface are not a stable device ABI.

* ``0x7f010000`` and ``0x7f010004`` are read-only: they expose the last
  written value and the number of writes, respectively.
* ``0x7f011000`` is the write alias. Reads return zero without side
  effects. One subtraction with this address as B and a word containing
  the negated value as A performs one 32-bit write of the desired value.
  The otherwise required B read sees zero, not the register's old value.

System reset clears the probe value and count. Only normal subtraction
operands use the probe; instruction fetch and byte console operations do
not. The probe does not define a virtio transport or IRQ interface.

Experimental virtio-mmio
~~~~~~~~~~~~~~~~~~~~~~~~

``-M esi,x-virtio-mmio=on`` adds three standard modern (version 2)
virtio-mmio transports; ``x-tools-disk=on`` additionally enables a fourth
block-only slot and requires ``x-virtio-mmio=on``. Both options default to off
and do not modify how ``-kernel`` or ``-bios`` loads an image. No block,
network or RNG device is auto-created. Use an explicit ``bus=`` to place
each device:

.. list-table::
   :header-rows: 1

   * - QEMU bus
     - Intended device
     - Read base
     - Write base
   * - ``virtio-mmio-bus.0``
     - ``virtio-blk-device``
     - ``0x60000000``
     - ``0x61000000``
   * - ``virtio-mmio-bus.1``
     - ``virtio-net-device``
     - ``0x60001000``
     - ``0x61001000``
   * - ``virtio-mmio-bus.2``
     - ``virtio-rng-device`` (explicit Linux ``rng-random`` backend) or optional second ``virtio-net-device``
     - ``0x60002000``
     - ``0x61002000``
   * - ``virtio-mmio-bus.3`` (requires ``x-tools-disk=on``)
     - ``virtio-blk-device`` only
     - ``0x60003000``
     - ``0x61003000``

Each window is 4 KiB. The read window exposes the standard register and
configuration offsets; a transport without a device reports device ID
zero. Writes to the read window are not allowed. All reads of the write
alias return zero without accessing the transport. To write a register
value, use the write alias as the subtraction destination and a word
containing the negated value as the source. One subtraction thus emits
one little-endian 32-bit store. The board accepts only aligned 32-bit
bus accesses, including for device configuration; guests must extract
narrow fields from aligned words. Within a window, padding after the
transport's 512-byte region reads as zero and ignores writes to the
write alias. The CPU permits these windows only for ordinary subtraction
operands, not instruction fetch, indirect pointer loads, or console
operations.

For example, to attach a scratch raw image and a restricted user-mode
network backend to a direct-boot capsule:

.. code-block:: console

   $ qemu-system-subleq -M esi,x-virtio-mmio=on -kernel capsule.raw \
       -display none -serial stdio -monitor none \
       -drive if=none,id=d0,format=raw,file=scratch.raw \
       -device virtio-blk-device,drive=d0,bus=virtio-mmio-bus.0 \
       -netdev user,id=n0,restrict=on \
       -device virtio-net-device,netdev=n0,bus=virtio-mmio-bus.1,mac=52:54:00:12:34:56

The first three slots support QEMU's userspace ``virtio-blk-device`` and
``virtio-net-device``; slot 2 also supports ``virtio-rng-device`` with an
explicit ready Linux ``rng-random`` backend. The optional fourth slot is
block-only. The NIC can use ordinary userspace networking, for
example ``user`` or ``tap`` with ``vhost=off``. Backends that can activate
vhost, including vhost-user, vhost-vDPA and reconnectable passt, are not
supported. The ESI slots reject them, other virtio frontend types and
``iommu_platform=on`` at plug time. ``x-idle-wakeup-v1=on`` enables a
board idle-wakeup protocol and ``x-idle-tick-coalesce-50ms=on`` enables
idle tick coalescing; see ``-machine esi,help`` for available options.

Virtqueue rings and buffers on these slots use a separate DMA address space
mapping only guest RAM at ``0x00000000`` through ``0x5fffffff``, inclusive.
It contains no firmware ROM, transport MMIO, IRQ bitmap or probe MMIO.
System reset does not change this mapping. This does not add a virtio feature
bit, restrict CPU accesses, or change virtio-mmio on other boards. The aperture
constrains the userspace virtio DMA path; it is not a whole-system isolation
boundary or a guarantee about arbitrary host backends.

An aligned 32-bit read at ``0x62000000`` samples the asserted transport
IRQ lines (bit ``n`` for slot ``n``). Reading the bitmap does not acknowledge
anything. Acknowledge interrupt status at each device's standard
``InterruptACK`` offset via its write alias. Acknowledging one device
leaves the other devices' lines unchanged.

Device completions do not enter the ESI CPU asynchronously. An aware guest
can sample this word at the existing instruction-count timer interrupt and
handle asserted devices there, or poll during boot. Without timer entry the
pending levels are still visible but are not dispatched. Sampling does not
modify the interrupted PC or timer state. This interface and the slot map
are experimental, not a stable platform ABI.

This machine does not yet provide a disk boot path, graphical
framebuffer or keyboard input, or migration and snapshots.
