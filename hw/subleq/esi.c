/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/cutils.h"
#include "qemu/datadir.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "cpu.h"
#include "hw/boards.h"
#include "hw/hotplug.h"
#include "hw/irq.h"
#include "hw/loader.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-properties-system.h"
#include "hw/virtio/virtio-blk.h"
#include "hw/virtio/virtio-mmio.h"
#include "hw/virtio/virtio-net.h"
#include "hw/virtio/virtio-rng.h"
#include "system/rng-random.h"
#include "system/address-spaces.h"
#include "system/qtest.h"
#include "system/reset.h"
#include "system/system.h"

#define TYPE_ESI_MACHINE MACHINE_TYPE_NAME("esi")
OBJECT_DECLARE_SIMPLE_TYPE(ESIMachineState, ESI_MACHINE)

typedef struct ESIVirtioMMIOSlot {
    MemoryRegion read;
    MemoryRegion write;
    VirtIOMMIOProxy *transport;
} ESIVirtioMMIOSlot;

struct ESIMachineState {
    MachineState parent;
    MemoryRegion rom;
    bool mmio_probe;
    MemoryRegion mmio_probe_read;
    MemoryRegion mmio_probe_write;
    uint32_t mmio_probe_value;
    uint32_t mmio_probe_writes;
    bool virtio_mmio;
    bool tools_disk;
    bool idle_wakeup_v1;
    bool idle_tick_coalesce_50ms;
    SubleqCPU *cpu;
    MemoryRegion idle_wakeup;
    MemoryRegion virtio_dma_ram;
    AddressSpace virtio_dma_as;
    ESIVirtioMMIOSlot virtio_slots[ESI_VIRTIO_MMIO_SLOTS];
    IRQState virtio_irqs[ESI_VIRTIO_MMIO_SLOTS];
    MemoryRegion virtio_pending;
    uint32_t virtio_irq_levels;
};

static MemTxResult esi_virtio_mmio_read(void *opaque, hwaddr addr,
                                        uint64_t *value, unsigned size,
                                        MemTxAttrs attrs)
{
    ESIVirtioMMIOSlot *slot = opaque;
    MemoryRegion *iomem = &slot->transport->iomem;

    if (addr + size > memory_region_size(iomem)) {
        *value = 0;
        return MEMTX_OK;
    }
    return memory_region_dispatch_read(iomem, addr, value,
                                       MO_LEUL, attrs);
}

static uint64_t esi_mmio_read_zero(void *opaque, hwaddr addr, unsigned size)
{
    /* A SUBLEQ destination read must not reach the transport. */
    return 0;
}

static MemTxResult esi_virtio_mmio_write(void *opaque, hwaddr addr,
                                         uint64_t value, unsigned size,
                                         MemTxAttrs attrs)
{
    ESIVirtioMMIOSlot *slot = opaque;
    MemoryRegion *iomem = &slot->transport->iomem;

    if (addr + size > memory_region_size(iomem)) {
        return MEMTX_OK;
    }
    return memory_region_dispatch_write(iomem, addr, value,
                                        MO_LEUL, attrs);
}

static bool esi_mmio_accept_read(void *opaque, hwaddr addr, unsigned size,
                                 bool is_write, MemTxAttrs attrs)
{
    return !is_write;
}

static const MemoryRegionOps esi_virtio_mmio_read_ops = {
    .read_with_attrs = esi_virtio_mmio_read,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
        .accepts = esi_mmio_accept_read,
    },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static const MemoryRegionOps esi_virtio_mmio_write_ops = {
    .read = esi_mmio_read_zero,
    .write_with_attrs = esi_virtio_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static void esi_virtio_irq_set(void *opaque, int slot, int level)
{
    ESIMachineState *s = opaque;
    uint32_t bit = 1u << slot;

    /* Virtio calls its SysBus IRQ sink with the BQL held. */
    if (level) {
        s->virtio_irq_levels |= bit;
        if (s->idle_wakeup_v1) {
            subleq_idle_notify(s->cpu);
        }
    } else {
        s->virtio_irq_levels &= ~bit;
    }
}

static uint64_t esi_virtio_pending_read(void *opaque, hwaddr addr,
                                        unsigned size)
{
    ESIMachineState *s = opaque;

    return s->virtio_irq_levels;
}

static const MemoryRegionOps esi_virtio_pending_ops = {
    .read = esi_virtio_pending_read,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
        .accepts = esi_mmio_accept_read,
    },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static void esi_virtio_pre_plug(HotplugHandler *handler, DeviceState *dev,
                                Error **errp)
{
    const char *type = object_get_typename(OBJECT(dev));

    if (!strcmp(type, TYPE_VIRTIO_RNG)) {
        ESIMachineState *s = ESI_MACHINE(handler);
        VirtIORNG *rng = VIRTIO_RNG(dev);
        RngBackend *backend = rng->conf.rng;

        if (qdev_get_parent_bus(dev) != BUS(&s->virtio_slots[2].transport->bus)) {
            error_setg(errp, "ESI virtio-rng requires slot 2");
            return;
        }
        if (!backend || strcmp(object_get_typename(OBJECT(backend)),
                               TYPE_RNG_RANDOM)) {
            error_setg(errp, "ESI virtio-rng requires explicit rng-random");
            return;
        }
#ifdef CONFIG_LINUX
        if (!rng_random_is_ready_urandom(RNG_RANDOM(backend))) {
            error_setg(errp, "ESI virtio-rng requires ready Linux urandom");
            return;
        }
#else
        error_setg(errp, "ESI virtio-rng requires Linux");
        return;
#endif
        if (rng->conf.max_bytes > 64) {
            error_setg(errp, "ESI virtio-rng requires max-bytes <= 64");
            return;
        }
    } else if (ESI_MACHINE(handler)->tools_disk &&
               qdev_get_parent_bus(dev) ==
               BUS(&ESI_MACHINE(handler)->virtio_slots[3].transport->bus) &&
               strcmp(type, TYPE_VIRTIO_BLK)) {
        error_setg(errp, "ESI tools-disk slot 3 requires virtio-blk");
        return;
    } else if (strcmp(type, TYPE_VIRTIO_BLK) && strcmp(type, TYPE_VIRTIO_NET)) {
        error_setg(errp, "ESI virtio-mmio supports only %s, %s and slot-2 %s",
                   TYPE_VIRTIO_BLK, TYPE_VIRTIO_NET, TYPE_VIRTIO_RNG);
        return;
    }

    if (virtio_host_has_feature(VIRTIO_DEVICE(dev), VIRTIO_F_IOMMU_PLATFORM)) {
        error_setg(errp, "ESI virtio-mmio does not support iommu_platform=on");
        return;
    }

    if (!strcmp(type, TYPE_VIRTIO_NET)) {
        const NICPeers *peers = &VIRTIO_NET(dev)->nic_conf.peers;
        int i;

        for (i = 0; i < peers->queues; i++) {
            NetClientState *peer = peers->ncs[i];

            /* A reconnectable peer can enable vhost after realization. */
            if (peer->info->get_vhost_net &&
                (peer->info->type != NET_CLIENT_DRIVER_TAP ||
                 peer->info->get_vhost_net(peer))) {
                error_setg(errp, "ESI virtio-mmio does not support vhost "
                           "network backends");
                return;
            }
        }
    }
}

static HotplugHandler *esi_get_hotplug_handler(MachineState *machine,
                                               DeviceState *dev)
{
    ESIMachineState *s = ESI_MACHINE(machine);
    BusState *bus = qdev_get_parent_bus(dev);
    unsigned i;

    for (i = 0; i < ARRAY_SIZE(s->virtio_slots); i++) {
        VirtIOMMIOProxy *transport = s->virtio_slots[i].transport;

        if (transport && bus == BUS(&transport->bus)) {
            /* Validate cold-plug without making the bus hotpluggable. */
            return HOTPLUG_HANDLER(s);
        }
    }
    return NULL;
}

/* Test the attached frontend's actual DMA mapping without issuing I/O. */
static bool esi_virtio_dma_qtest(CharFrontend *chr, gchar **words)
{
    ESIMachineState *s = ESI_MACHINE(current_machine);
    VirtIODevice *vdev;
    unsigned slot;
    uint64_t addr, len;
    bool rd, wr;

    if (strcmp(words[0], "esi-virtio-dma-valid")) {
        return false;
    }
    if (g_strv_length(words) != 4 ||
        qemu_strtoui(words[1], NULL, 0, &slot) ||
        slot >= ARRAY_SIZE(s->virtio_slots) ||
        !s->virtio_slots[slot].transport ||
        qemu_strtou64(words[2], NULL, 0, &addr) ||
        qemu_strtou64(words[3], NULL, 0, &len) ||
        !len || len > 16 || addr > UINT64_MAX - (len - 1)) {
        qtest_sendf(chr, "FAIL Invalid slot, address or length\n");
        return true;
    }
    vdev = virtio_bus_get_device(&s->virtio_slots[slot].transport->bus);
    if (!vdev) {
        qtest_sendf(chr, "FAIL No virtio device in slot %u\n", slot);
        return true;
    }
    rd = address_space_access_valid(vdev->dma_as, addr, len, false,
                                    MEMTXATTRS_UNSPECIFIED);
    wr = address_space_access_valid(vdev->dma_as, addr, len, true,
                                    MEMTXATTRS_UNSPECIFIED);
    qtest_sendf(chr, "OK %d %d\n", rd, wr);
    return true;
}

static void esi_virtio_mmio_init(ESIMachineState *s)
{
    MachineState *machine = MACHINE(s);
    MemoryRegion *sysmem = get_system_memory();
    unsigned i;

    /* Machine-lifetime mapping: reset only resets the devices using it. */
    memory_region_init_alias(&s->virtio_dma_ram, OBJECT(s),
                             "esi.virtio-dma-ram", machine->ram, 0,
                             machine->ram_size);
    address_space_init(&s->virtio_dma_as, &s->virtio_dma_ram, "esi.virtio-dma");
    if (qtest_enabled()) {
        qtest_set_command_cb(esi_virtio_dma_qtest);
    }

    memory_region_init_io(&s->virtio_pending, OBJECT(s),
                          &esi_virtio_pending_ops, s, "esi.virtio-pending", 4);
    memory_region_add_subregion(sysmem, ESI_VIRTIO_MMIO_PENDING,
                                &s->virtio_pending);

    for (i = 0; i < (s->tools_disk ? 4 : 3); i++) {
        ESIVirtioMMIOSlot *slot = &s->virtio_slots[i];
        g_autofree char *name = g_strdup_printf("virtio-mmio-%u", i);
        g_autofree char *irq_name = g_strdup_printf("virtio-irq-%u", i);
        DeviceState *dev = qdev_new(TYPE_VIRTIO_MMIO);
        hwaddr offset = i * ESI_VIRTIO_MMIO_STRIDE;

        object_property_add_child(OBJECT(s), name, OBJECT(dev));
        virtio_mmio_set_default_dma_as(VIRTIO_MMIO(dev), &s->virtio_dma_as);
        qdev_prop_set_bit(dev, "force-legacy", false);
        /* Only the board's split windows are mapped in system memory. */
        qdev_prop_set_bit(dev, "format_transport_address", false);
        qdev_prop_set_bit(dev, "ioeventfd", false);
        sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
        slot->transport = VIRTIO_MMIO(dev);
        qemu_init_irq_child(OBJECT(s), irq_name, &s->virtio_irqs[i],
                            esi_virtio_irq_set, s, i);
        sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, &s->virtio_irqs[i]);

        memory_region_init_io(&slot->read, OBJECT(s), &esi_virtio_mmio_read_ops,
                              slot, "esi.virtio-mmio-read",
                              ESI_VIRTIO_MMIO_STRIDE);
        memory_region_init_io(&slot->write, OBJECT(s),
                              &esi_virtio_mmio_write_ops, slot,
                              "esi.virtio-mmio-write", ESI_VIRTIO_MMIO_STRIDE);
        memory_region_add_subregion(sysmem, ESI_VIRTIO_MMIO_READ + offset,
                                    &slot->read);
        memory_region_add_subregion(sysmem, ESI_VIRTIO_MMIO_WRITE + offset,
                                    &slot->write);
    }
}

static uint64_t esi_mmio_probe_read(void *opaque, hwaddr addr, unsigned size)
{
    ESIMachineState *s = opaque;

    return addr == 0 ? s->mmio_probe_value : s->mmio_probe_writes;
}

static void esi_mmio_probe_write(void *opaque, hwaddr addr, uint64_t value,
                                 unsigned size)
{
    ESIMachineState *s = opaque;

    s->mmio_probe_value = value;
    s->mmio_probe_writes++;
}

static const MemoryRegionOps esi_mmio_probe_read_ops = {
    .read = esi_mmio_probe_read,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
        .accepts = esi_mmio_accept_read,
    },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static const MemoryRegionOps esi_mmio_probe_write_ops = {
    .read = esi_mmio_read_zero,
    .write = esi_mmio_probe_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static void esi_mmio_probe_reset(void *opaque)
{
    ESIMachineState *s = opaque;

    s->mmio_probe_value = 0;
    s->mmio_probe_writes = 0;
}

static void esi_mmio_probe_init(ESIMachineState *s)
{
    MemoryRegion *sysmem = get_system_memory();

    memory_region_init_io(&s->mmio_probe_read, OBJECT(s),
                          &esi_mmio_probe_read_ops, s,
                          "esi.mmio-probe-read", ESI_MMIO_PROBE_READ_SIZE);
    memory_region_init_io(&s->mmio_probe_write, OBJECT(s),
                          &esi_mmio_probe_write_ops, s,
                          "esi.mmio-probe-write", 4);
    memory_region_add_subregion(sysmem, ESI_MMIO_PROBE_READ,
                                &s->mmio_probe_read);
    memory_region_add_subregion(sysmem, ESI_MMIO_PROBE_WRITE,
                                &s->mmio_probe_write);
    qemu_register_reset(esi_mmio_probe_reset, s);
}

static uint64_t esi_idle_read(void *opaque, hwaddr addr, unsigned size)
{
    return addr == 4 ? 1 : 0;
}

static void esi_idle_write(void *opaque, hwaddr addr, uint64_t value,
                           unsigned size)
{
    /* The CPU helper enters idle after committing the instruction and PC. */
}

static const MemoryRegionOps esi_idle_ops = {
    .read = esi_idle_read,
    .write = esi_idle_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static void esi_load_firmware(ESIMachineState *s, const char *name)
{
    g_autofree char *path = qemu_find_file(QEMU_FILE_TYPE_BIOS, name);
    g_autofree uint8_t *image = g_malloc(ESI_ROM_SIZE + 1);
    ssize_t size = load_image_size(path ? path : name, image, ESI_ROM_SIZE + 1);

    /* Read at most one byte beyond the aperture to detect an oversized file. */
    if (size < 0) {
        error_report("Could not read ESI firmware '%s'", name);
        exit(EXIT_FAILURE);
    }
    if (size < SUBLEQ_INSN_SIZE || size > ESI_ROM_SIZE) {
        error_report("ESI firmware '%s' must be %d to %" PRId64 " bytes",
                     name, SUBLEQ_INSN_SIZE, ESI_ROM_SIZE);
        exit(EXIT_FAILURE);
    }

    memory_region_init_rom_nomigrate(&s->rom, OBJECT(s), "esi.rom",
                                     ESI_ROM_SIZE, &error_fatal);
    memory_region_add_subregion(get_system_memory(), ESI_ROM_BASE, &s->rom);
    rom_add_blob_fixed("esi.firmware", image, size, ESI_ROM_BASE);
}

static void esi_machine_init(MachineState *machine)
{
    ESIMachineState *s = ESI_MACHINE(machine);
    DeviceState *cpu;
    Chardev *chr = serial_hd(0);

    if (machine->ram_size != SUBLEQ_RAM_SIZE) {
        error_report("The ESI machine requires 1536 MiB of RAM");
        exit(EXIT_FAILURE);
    }
    if (s->idle_tick_coalesce_50ms && !s->idle_wakeup_v1) {
        error_report("x-idle-tick-coalesce-50ms requires x-idle-wakeup-v1=on");
        exit(EXIT_FAILURE);
    }
    if (machine->kernel_filename && machine->firmware) {
        error_report("ESI -bios and -kernel are mutually exclusive");
        exit(EXIT_FAILURE);
    }
    memory_region_add_subregion(get_system_memory(), 0, machine->ram);

    if (machine->firmware) {
        esi_load_firmware(s, machine->firmware);
    } else if (machine->kernel_filename) {
        load_image_targphys(machine->kernel_filename, 0, SUBLEQ_RAM_SIZE,
                            &error_fatal);
    }

    cpu = qdev_new(machine->cpu_type);
    object_property_add_child(OBJECT(machine), "cpu", OBJECT(cpu));
    qdev_prop_set_uint32(cpu, "reset-vector",
                         machine->firmware ? ESI_ROM_BASE : 0);
    qdev_prop_set_bit(cpu, "x-idle-wakeup-v1", s->idle_wakeup_v1);
    qdev_prop_set_bit(cpu, "x-idle-tick-coalesce-50ms",
                      s->idle_tick_coalesce_50ms);
    s->cpu = SUBLEQ_CPU(cpu);
    s->cpu->virtio_irq_levels = &s->virtio_irq_levels;
    if (s->idle_wakeup_v1) {
        memory_region_init_io(&s->idle_wakeup, OBJECT(s), &esi_idle_ops, s,
                              "esi.idle-wakeup-v1", 8);
        memory_region_add_subregion(get_system_memory(), ESI_IDLE_WAKEUP_WAIT,
                                    &s->idle_wakeup);
    }
    if (s->mmio_probe) {
        esi_mmio_probe_init(s);
    }
    qdev_prop_set_bit(cpu, "x-mmio-probe", s->mmio_probe);
    if (s->tools_disk && !s->virtio_mmio) {
        error_report("x-tools-disk requires x-virtio-mmio=on");
        exit(EXIT_FAILURE);
    }
    if (s->virtio_mmio) {
        esi_virtio_mmio_init(s);
    }
    qdev_prop_set_bit(cpu, "x-virtio-mmio", s->virtio_mmio);
    qdev_prop_set_bit(cpu, "x-tools-disk", s->tools_disk);
    if (chr) {
        qdev_prop_set_chr(cpu, "chardev", chr);
    }
    qdev_realize_and_unref(cpu, NULL, &error_fatal);
    qemu_register_resettable(OBJECT(cpu));
}

static bool esi_machine_get_mmio_probe(Object *obj, Error **errp)
{
    return ESI_MACHINE(obj)->mmio_probe;
}

static void esi_machine_set_mmio_probe(Object *obj, bool value, Error **errp)
{
    if (phase_check(PHASE_MACHINE_INITIALIZED)) {
        error_setg(errp, "x-mmio-probe cannot be changed after initialization");
        return;
    }
    ESI_MACHINE(obj)->mmio_probe = value;
}

static bool esi_machine_get_virtio_mmio(Object *obj, Error **errp)
{
    return ESI_MACHINE(obj)->virtio_mmio;
}

static void esi_machine_set_virtio_mmio(Object *obj, bool value, Error **errp)
{
    if (phase_check(PHASE_MACHINE_INITIALIZED)) {
        error_setg(errp,
                   "x-virtio-mmio cannot be changed after initialization");
        return;
    }
    ESI_MACHINE(obj)->virtio_mmio = value;
}

static bool esi_machine_get_tools_disk(Object *obj, Error **errp)
{
    return ESI_MACHINE(obj)->tools_disk;
}

static void esi_machine_set_tools_disk(Object *obj, bool value, Error **errp)
{
    if (phase_check(PHASE_MACHINE_INITIALIZED)) {
        error_setg(errp, "x-tools-disk cannot be changed after initialization");
        return;
    }
    ESI_MACHINE(obj)->tools_disk = value;
}

static bool esi_machine_get_idle_wakeup(Object *obj, Error **errp)
{
    return ESI_MACHINE(obj)->idle_wakeup_v1;
}

static void esi_machine_set_idle_wakeup(Object *obj, bool value, Error **errp)
{
    if (phase_check(PHASE_MACHINE_INITIALIZED)) {
        error_setg(errp, "x-idle-wakeup-v1 cannot be changed after initialization");
        return;
    }
    ESI_MACHINE(obj)->idle_wakeup_v1 = value;
}

static bool esi_machine_get_idle_coalesce(Object *obj, Error **errp)
{
    return ESI_MACHINE(obj)->idle_tick_coalesce_50ms;
}

static void esi_machine_set_idle_coalesce(Object *obj, bool value, Error **errp)
{
    if (phase_check(PHASE_MACHINE_INITIALIZED)) {
        error_setg(errp, "x-idle-tick-coalesce-50ms cannot change after initialization");
        return;
    }
    ESI_MACHINE(obj)->idle_tick_coalesce_50ms = value;
}

static void esi_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    HotplugHandlerClass *hc = HOTPLUG_HANDLER_CLASS(oc);

    mc->get_hotplug_handler = esi_get_hotplug_handler;
    hc->pre_plug = esi_virtio_pre_plug;
    mc->desc = "Eternal Software Initiative (ESI)";
    mc->init = esi_machine_init;
    mc->is_default = true;
    mc->default_cpu_type = TYPE_SUBLEQ_CPU;
    mc->default_ram_id = "esi.ram";
    mc->default_ram_size = SUBLEQ_RAM_SIZE;
    mc->max_cpus = 1;
    mc->no_cdrom = true;
    mc->no_floppy = true;
    mc->no_parallel = true;

    object_class_property_add_bool(oc, "x-mmio-probe",
                                  esi_machine_get_mmio_probe,
                                  esi_machine_set_mmio_probe);
    object_class_property_set_description(oc, "x-mmio-probe",
                                          "Enable experimental MMIO probe");
    object_class_property_add_bool(oc, "x-virtio-mmio",
                                   esi_machine_get_virtio_mmio,
                                   esi_machine_set_virtio_mmio);
    object_class_property_set_description(oc, "x-virtio-mmio",
                                          "Enable experimental virtio-mmio");
    object_class_property_add_bool(oc, "x-tools-disk",
                                   esi_machine_get_tools_disk,
                                   esi_machine_set_tools_disk);
    object_class_property_set_description(oc, "x-tools-disk",
                                          "Add opt-in fourth virtio-mmio block slot");
    object_class_property_add_bool(oc, "x-idle-wakeup-v1",
                                   esi_machine_get_idle_wakeup,
                                   esi_machine_set_idle_wakeup);
    object_class_property_set_description(oc, "x-idle-wakeup-v1",
                                          "Enable interruptible idle MMIO ABI v1");
    object_class_property_add_bool(oc, "x-idle-tick-coalesce-50ms",
                                   esi_machine_get_idle_coalesce,
                                   esi_machine_set_idle_coalesce);
    object_class_property_set_description(oc, "x-idle-tick-coalesce-50ms",
                                          "Experimental idle-only 50ms tick deadline");
}

static const TypeInfo esi_machine_types[] = {
    {
        .name = TYPE_ESI_MACHINE,
        .parent = TYPE_MACHINE,
        .instance_size = sizeof(ESIMachineState),
        .class_init = esi_machine_class_init,
        .interfaces = (const InterfaceInfo[]) {
            { TYPE_HOTPLUG_HANDLER },
            { }
        },
    },
};

DEFINE_TYPES(esi_machine_types)
