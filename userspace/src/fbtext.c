/*
 * fbtext -- unconditionally restore the console to KD_TEXT.
 *
 * A framebuffer app that takes over the VT (KDSETMODE KD_GRAPHICS) can
 * catch SIGINT/SIGTERM and revert it on a graceful stop, but SIGKILL, a
 * crash, or an OOM-kill can't be caught by any process -- the console is
 * left stuck in KD_GRAPHICS with no visible shell. Run this unconditionally
 * after any such app exits (see fbrun) as a general safety net, rather than
 * duplicating the same ioctl in every app's own cleanup path.
 */
#include <fcntl.h>
#include <linux/kd.h>
#include <sys/ioctl.h>
#include <unistd.h>

int main(void)
{
    int fd = open("/dev/tty0", O_RDWR);
    if (fd < 0)
        fd = open("/dev/console", O_RDWR);
    if (fd < 0)
        return 1;
    ioctl(fd, KDSETMODE, KD_TEXT);
    close(fd);
    return 0;
}
