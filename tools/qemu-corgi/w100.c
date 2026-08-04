/*
 * QEMU model of the ATI Imageon "W100" 2D graphics companion chip, as wired
 * into the Sharp Zaurus Corgi/Shepherd/Husky (SL-C7xx/SL-C860) PXA25x
 * boards via nCS2.
 *
 * This models only what the Linux `w100fb` driver needs to probe the chip
 * and drive a fixed 640x480 RGB565 linear framebuffer: the CHIP_ID probe
 * register, the mode-set registers it writes during init
 * (GRAPHIC_CTRL/OFFSET/PITCH), and a plain VRAM window. The 2D acceleration
 * engine (BitBLT, line draw, the RBBM_STATUS FIFO/busy bits real drivers
 * poll for accel completion) is out of scope -- every fbdev-based
 * userspace program on this device (otXash, otCraft, otQuake) just mmaps
 * the framebuffer and writes pixels directly, the same as it would against
 * any other Linux fbdev.
 *
 * Physical layout (see drivers/video/fbdev/w100fb.h and the Corgi board
 * file, arch/arm/mach-pxa/corgi.c, in the target kernel):
 *   +0x00000  CFG space   (16 bytes,  reachable even while chip is asleep)
 *   +0x10000  register space (8 KiB, mmXXX registers)
 *   +0x800000 framebuffer window (2 MiB VRAM, W100_FB_BASE)
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 */

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

/* offsets within the register window, from w100fb.h */
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
    uint8_t *vram_ptr; /* direct pointer, see w100_update_display */

    QemuConsole *con;

    uint8_t cfg_bytes[W100_CFG_LEN];
    uint32_t regs[W100_REG_LEN / 4];

    uint32_t fb_offset; /* byte offset into vram currently scanned out */
    uint32_t fb_pitch;  /* bytes per row, as programmed by the driver */
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
        /* Report a completely empty command FIFO and an idle engine.
         *
         * cmdfifo_avail is bits 0:6 and w100_fifo_wait() spins until it is
         * >= the number of entries wanted, so returning 0 here does not mean
         * "idle", it means "never ready": the driver burns its full
         * 2,000,000-iteration budget and prints "w100fb: FIFO Timeout!". That
         * cost ~8 s of every boot. 0x7f is the max the field can hold, so
         * every wait returns on the first read, and with all the *_busy bits
         * clear w100fb_sync() returns immediately too. */
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
        /* color_depth/portrait/enable bits: nothing else to do, the
         * driver only ever drives this in 16bpp mode on this board */
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

/*
 * Recover the top-left origin of the logical framebuffer from the raw
 * mmGRAPHIC_OFFSET the driver programmed.
 *
 * The register is NOT a plain top-left byte offset. w100fb_scanout_offset()
 * (drivers/video/fbdev/w100fb.c) points the CRTC at the *start of the last
 * row* whenever the axes are swapped -- which is Corgi/Husky's normal state,
 * because the panel is natively 480x640 portrait and the chip rotates 90
 * degrees on scanout to present the 640x480 landscape the user actually
 * sees. The hardware then walks upward from there.
 *
 * The driver's own comment records the exact values measured on real
 * hardware, which is what made this diagnosable:
 *
 *   yoffset=0   -> GRAPHIC_OFFSET = 0x00895b00   (0x800000 + 640*479*2)
 *   yoffset=480 -> GRAPHIC_OFFSET = 0x0092bb00   (one full frame further on)
 *
 * So subtract the rotation base to get back to the pixel the user sees at
 * the top-left. We present the un-rotated landscape image rather than
 * modelling the panel's physical portrait scan: that is what the device
 * looks like in the hand, and what every fbdev client here draws.
 */
static uint32_t w100_frame_start(W100State *s)
{
    uint32_t portrait = (s->regs[mmGRAPHIC_CTRL / 4] >> 3) & 3; /* bits 3:4 */
    uint32_t base = 0;
    uint32_t start = s->fb_offset;

    if (portrait == 1 || portrait == 3) { /* 90 / 270: axes swapped */
        base = (W100_DISP_HEIGHT - 1) * s->fb_pitch;
    }

    start = (start >= base) ? start - base : 0;

    /* never scan past the end of VRAM, whatever the guest programmed */
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

    /* Direct pointer blit every refresh, rather than
     * framebuffer_update_display()'s dirty-bitmap-tracked partial redraw:
     * that helper asserts (tlb_reset_dirty_range_all) on this device's RAM
     * region. A fixed 640x480x16bpp console redrawn in full each refresh is
     * cheap enough that the dirty-tracking optimisation isn't worth it. */
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

    /* Three independent top-level sysbus regions (cfg=0, regs=1, vram=2)
     * rather than one container with subregions -- vram nested inside a
     * plain container crashed tlb_reset_dirty_range_all() when dirty
     * logging was enabled on it (couldn't resolve its RAMBlock through
     * the extra container level). The machine code places all three at
     * their real fixed offsets within the nCS2 window (see corgi.c). */

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
