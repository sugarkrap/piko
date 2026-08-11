
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/arm/pxa.h"
#include "hw/arm/boot.h"
#include "hw/arm/sharpsl.h"
#include "hw/boards.h"
#include "hw/irq.h"
#include "hw/sysbus.h"
#include "qom/object.h"

#define CORGI_RAM       0x04000000
#define W100_BASE       0x08000000
#define W100_CFG_OFFSET 0x000000
#define W100_REG_OFFSET 0x010000
#define W100_FB_OFFSET  0x800000

static struct arm_boot_info corgi_binfo = {
    .loader_start = PXA2XX_SDRAM_BASE,
    .ram_size = CORGI_RAM,
};

static void corgi_common_init(MachineState *machine, int board_id)
{
    PXA2xxState *mpu;
    DeviceState *w100;
    SysBusDevice *w100_sbd;
    DeviceState *scoop;

    w100 = qdev_new("w100");
    w100->id = g_strdup("w100");
    w100_sbd = SYS_BUS_DEVICE(w100);
    sysbus_realize_and_unref(w100_sbd, &error_fatal);
    sysbus_mmio_map(w100_sbd, 0, W100_BASE + W100_CFG_OFFSET);
    sysbus_mmio_map(w100_sbd, 1, W100_BASE + W100_REG_OFFSET);
    sysbus_mmio_map(w100_sbd, 2, W100_BASE + W100_FB_OFFSET);

    mpu = pxa255_init(corgi_binfo.ram_size);

    scoop = sysbus_create_simple("scoop", 0x10800000, NULL);
    (void)scoop;

    qemu_irq_raise(qdev_get_gpio_in(mpu->gpio, 7));

    corgi_binfo.board_id = board_id;
    arm_load_kernel(mpu->cpu, machine, &corgi_binfo);
    sl_bootparam_write(SL_PXA_PARAM_BASE);
}

static void corgi_init(MachineState *machine)
{
    corgi_common_init(machine, 0x1a7);
}

static void husky_init(MachineState *machine)
{
    corgi_common_init(machine, 0x21f);
}

static void corgi_machine_class_init(ObjectClass *oc, void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "Sharp SL-C700 (Corgi) PDA (PXA255)";
    mc->init = corgi_init;
    mc->ignore_memory_transaction_failures = true;
    mc->deprecation_reason = "no NAND/rootfs modeled yet; boot via -initrd";
}

static const TypeInfo corgi_machine_type = {
    .name = MACHINE_TYPE_NAME("corgi"),
    .parent = TYPE_MACHINE,
    .class_init = corgi_machine_class_init,
};

static void husky_machine_class_init(ObjectClass *oc, void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "Sharp SL-C860 (Husky) PDA (PXA255)";
    mc->init = husky_init;
    mc->ignore_memory_transaction_failures = true;
    mc->deprecation_reason = "no NAND/rootfs modeled yet; boot via -initrd";
}

static const TypeInfo husky_machine_type = {
    .name = MACHINE_TYPE_NAME("husky"),
    .parent = TYPE_MACHINE,
    .class_init = husky_machine_class_init,
};

static void corgi_machine_init(void)
{
    type_register_static(&corgi_machine_type);
    type_register_static(&husky_machine_type);
}

type_init(corgi_machine_init)
