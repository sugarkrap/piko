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
