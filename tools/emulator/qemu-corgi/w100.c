
#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "hw/sysbus.h"
#include "migration/vmstate.h"
#include "ui/console.h"
#include "qom/object.h"

#define TYPE_W100 "w100"
OBJECT_DECLARE_SIMPLE_TYPE(W100State, W100)

#define W100_CFG_BASE     0x000000
#define W100_CFG_LEN      0x10
#define W100_REG_BASE     0x010000
#define W100_REG_LEN      0x2000
#define W100_FB_BASE       0x800000
#define W100_VRAM_SIZE     (2 * MiB)

#define mmCHIP_ID          0x0000
#define mmRBBM_STATUS      0x0140
#define mmGRAPHIC_CTRL     0x0414
#define mmGRAPHIC_OFFSET   0x0418
#define mmGRAPHIC_PITCH    0x041c

#define CHIP_ID_W100       0x57411002

#define W100_DISP_WIDTH    640
#define W100_DISP_HEIGHT   480

struct W100State {
    SysBusDevice parent_obj;

    MemoryRegion cfg;
    MemoryRegion regs_mr;
    MemoryRegion vram;
    uint8_t *vram_ptr;

    QemuConsole *con;

    uint8_t cfg_bytes[W100_CFG_LEN];
    uint32_t regs[W100_REG_LEN / 4];

    uint32_t fb_offset;
    uint32_t fb_pitch;
};

static uint64_t w100_cfg_read(void *opaque, hwaddr addr, unsigned size)
{
    W100State *s = opaque;
    uint64_t val = 0;
    unsigned i;

    for (i = 0; i < size && addr + i < W100_CFG_LEN; i++) {
        val |= (uint64_t)s->cfg_bytes[addr + i] << (8 * i);
    }
    return val;
}

static void w100_cfg_write(void *opaque, hwaddr addr, uint64_t val,
                            unsigned size)
{
    W100State *s = opaque;
    unsigned i;

    for (i = 0; i < size && addr + i < W100_CFG_LEN; i++) {
        s->cfg_bytes[addr + i] = (val >> (8 * i)) & 0xff;
    }
}

static const MemoryRegionOps w100_cfg_ops = {
    .read = w100_cfg_read,
    .write = w100_cfg_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static uint64_t w100_reg_read(void *opaque, hwaddr addr, unsigned size)
{
    W100State *s = opaque;
    hwaddr idx = (addr & ~3) / 4;

    if (idx >= ARRAY_SIZE(s->regs)) {
        return 0;
    }

    switch (addr & ~3) {
    case mmCHIP_ID:
        return CHIP_ID_W100;
    case mmRBBM_STATUS:
        return 0x7f;
    default:
        return s->regs[idx];
    }
}

static void w100_reg_write(void *opaque, hwaddr addr, uint64_t val,
                            unsigned size)
{
    W100State *s = opaque;
    hwaddr off = addr & ~3;
    hwaddr idx = off / 4;

    if (idx >= ARRAY_SIZE(s->regs)) {
        return;
    }

    s->regs[idx] = val;

    switch (off) {
    case mmGRAPHIC_OFFSET:
        s->fb_offset = (val >= W100_FB_BASE) ? val - W100_FB_BASE : 0;
        break;
    case mmGRAPHIC_PITCH:
        s->fb_pitch = val;
        break;
    case mmGRAPHIC_CTRL:
        break;
    default:
        break;
    }
}

static const MemoryRegionOps w100_reg_ops = {
    .read = w100_reg_read,
    .write = w100_reg_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static void w100_draw_row_rgb565(void *opaque, uint8_t *d, const uint8_t *src,
                                  int width, int dest_pitch)
{
    const uint16_t *s16 = (const uint16_t *)src;
    uint32_t *d32 = (uint32_t *)d;
    int x;

    for (x = 0; x < width; x++) {
        uint16_t p = s16[x];
        uint8_t r = ((p >> 11) & 0x1f) * 255 / 31;
        uint8_t g = ((p >> 5) & 0x3f) * 255 / 63;
        uint8_t b = (p & 0x1f) * 255 / 31;

        d32[x] = (0xffu << 24) | (r << 16) | (g << 8) | b;
    }
}

static uint32_t w100_frame_start(W100State *s)
{
    uint32_t portrait = (s->regs[mmGRAPHIC_CTRL / 4] >> 3) & 3;
    uint32_t base = 0;
    uint32_t start = s->fb_offset;

    if (portrait == 1 || portrait == 3) {
        base = (W100_DISP_HEIGHT - 1) * s->fb_pitch;
    }

    start = (start >= base) ? start - base : 0;

    if (start + (size_t)W100_DISP_HEIGHT * s->fb_pitch > W100_VRAM_SIZE) {
        start = 0;
    }
    return start;
}

static void w100_update_display(void *opaque)
{
    W100State *s = opaque;
    DisplaySurface *surface = qemu_console_surface(s->con);
    uint32_t start;
    uint8_t *dst;
    int y;

    if (!s->fb_pitch || !surface) {
        return;
    }

    start = w100_frame_start(s);
    dst = surface_data(surface);
    for (y = 0; y < W100_DISP_HEIGHT; y++) {
        const uint8_t *src = s->vram_ptr + start + (size_t)y * s->fb_pitch;
        w100_draw_row_rgb565(s, dst + (size_t)y * surface_stride(surface),
                              src, W100_DISP_WIDTH, surface_stride(surface));
    }

    dpy_gfx_update(s->con, 0, 0, W100_DISP_WIDTH, W100_DISP_HEIGHT);
}

static const GraphicHwOps w100_ops = {
    .gfx_update = w100_update_display,
};

static void w100_realize(DeviceState *dev, Error **errp)
{
    W100State *s = W100(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    memory_region_init_io(&s->cfg, OBJECT(dev), &w100_cfg_ops, s,
                           "w100.cfg", W100_CFG_LEN);
    sysbus_init_mmio(sbd, &s->cfg);

    memory_region_init_io(&s->regs_mr, OBJECT(dev), &w100_reg_ops, s,
                           "w100.regs", W100_REG_LEN);
    sysbus_init_mmio(sbd, &s->regs_mr);

    memory_region_init_ram(&s->vram, OBJECT(dev), "w100.vram",
                            W100_VRAM_SIZE, &error_fatal);
    sysbus_init_mmio(sbd, &s->vram);
    s->vram_ptr = memory_region_get_ram_ptr(&s->vram);

    s->fb_pitch = W100_DISP_WIDTH * 2;

    s->con = graphic_console_init(dev, 0, &w100_ops, s);
    qemu_console_resize(s->con, W100_DISP_WIDTH, W100_DISP_HEIGHT);
}

static void w100_reset(DeviceState *dev)
{
    W100State *s = W100(dev);

    memset(s->cfg_bytes, 0, sizeof(s->cfg_bytes));
    memset(s->regs, 0, sizeof(s->regs));
    s->fb_offset = 0;
    s->fb_pitch = W100_DISP_WIDTH * 2;
}

static const VMStateDescription vmstate_w100 = {
    .name = "w100",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(cfg_bytes, W100State, W100_CFG_LEN),
        VMSTATE_UINT32_ARRAY(regs, W100State, W100_REG_LEN / 4),
        VMSTATE_UINT32(fb_offset, W100State),
        VMSTATE_UINT32(fb_pitch, W100State),
        VMSTATE_END_OF_LIST()
    }
};

static void w100_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = w100_realize;
    dc->reset = w100_reset;
    dc->vmsd = &vmstate_w100;
}

static const TypeInfo w100_info = {
    .name = TYPE_W100,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(W100State),
    .class_init = w100_class_init,
};

static void w100_register_types(void)
{
    type_register_static(&w100_info);
}

type_init(w100_register_types)
