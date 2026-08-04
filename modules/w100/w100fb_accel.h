/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * w100fb 2D acceleration ioctls.
 *
 * Generalized fillrect/copyarea, usable against ANY surface within the
 * framebuffer's own mapped memory -- not just the live/visible framebuffer
 * fbcon draws to. Lets a process holding /dev/fb0 open keep a sprite/tile
 * cache in spare VRAM and have the chip's 2D engine composite it into the
 * framebuffer, instead of a CPU memcpy through write-combined-but-still-
 * off-chip memory (see the w100fb_mmap() comment in the driver for what
 * that costs).
 *
 * This header is included by both the kernel driver
 * (modules/w100/w100fb_patched.c, copied to drivers/video/fbdev/w100fb.c
 * by tools/setup-kernel-src.sh, which also copies this file in as
 * include/video/w100fb_accel.h) and by any userspace consumer that wants
 * these ioctls -- there is exactly one copy of the command numbers and
 * argument layout, not two that could drift.
 *
 * offsets are BYTE offsets from the framebuffer's own base
 * (fb_fix_screeninfo.smem_start) -- 0 is the first byte of the live
 * framebuffer itself, same address space FBIOPAN_DISPLAY's yoffset already
 * reaches into. Every surface is implicitly 16 bits per pixel (RGB565),
 * the only format this driver's GUI programming (DstType_16Bpp_444)
 * supports -- there is no format field.
 *
 * SCISSOR: the engine's clip rectangle is fixed to the CURRENT display
 * mode's own [0,0]-[xres,yres] for every op, on-screen or off -- see the
 * comment above w100_accel_fillrect() in the driver. A rect that does not
 * fit inside that box is rejected with -EINVAL rather than silently
 * clipped: there is no per-call scissor in this version, so a surface
 * logically bigger than the current mode has to be filled/blitted through
 * in mode-sized tiles.
 *
 * BACKING: an offset+pitch*height that reaches past what is actually
 * mapped in right now (fb_fix_screeninfo.smem_len -- see w100fb_set_par(),
 * which sizes this to match whatever xres_virtual/yres_virtual was last
 * accepted, internal SRAM or external SDRAM, NOT just the visible frame)
 * is rejected with -EINVAL. Claim the memory first -- FBIOPUT_VSCREENINFO
 * with a big enough yres_virtual -- before targeting an offset past the
 * visible frame; there is no separate allocator here, only what fbdev's
 * own virtual-buffer accounting already tracks.
 *
 * Designed and logic-reviewed against the real driver source in this repo.
 * NOT compiled or run: this repo has no kernel build tree, so there is no
 * way to `make modules` this, let alone boot-test it, from here. Before
 * trusting it: cross-compile it, then smoke-test W100FB_IOC_FILL/BLIT/SYNC
 * against the live framebuffer surface (offset 0, safe -- fbcon already
 * blits there) before trusting an off-screen surface.
 */
#ifndef _W100FB_ACCEL_H
#define _W100FB_ACCEL_H

#include <linux/types.h>
#include <linux/ioctl.h>

struct w100fb_fill_args {
	__u32 dst_offset;	/* bytes from smem_start */
	__u32 dst_pitch;	/* bytes per row */
	__u32 x, y;
	__u32 width, height;
	__u32 color;		/* RGB565 */
};

struct w100fb_blit_args {
	__u32 src_offset;
	__u32 src_pitch;
	__u32 dst_offset;
	__u32 dst_pitch;
	__u32 sx, sy;
	__u32 dx, dy;
	__u32 width, height;
};

#define W100FB_IOC_MAGIC	'w'
#define W100FB_IOC_FILL		_IOW(W100FB_IOC_MAGIC, 0, struct w100fb_fill_args)
#define W100FB_IOC_BLIT		_IOW(W100FB_IOC_MAGIC, 1, struct w100fb_blit_args)
#define W100FB_IOC_SYNC		_IO(W100FB_IOC_MAGIC, 2)

#endif
