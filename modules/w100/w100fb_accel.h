#ifndef _W100FB_ACCEL_H
#define _W100FB_ACCEL_H

#include <linux/types.h>
#include <linux/ioctl.h>

struct w100fb_fill_args {
	__u32 dst_offset;
	__u32 dst_pitch;
	__u32 x, y;
	__u32 width, height;
	__u32 color;
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
