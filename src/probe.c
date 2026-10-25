// Probe IIO DMABUF ioctl support on the running kernel. Harmless: attaches fd -1.
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#define IIO_BUFFER_GET_FD_IOCTL        _IOWR('i', 0x91, int)
#define IIO_BUFFER_DMABUF_ATTACH_IOCTL _IOW('i', 0x92, int)
int main(int argc, char **argv) {
    const char *dev = argc > 1 ? argv[1] : "/dev/iio:device3";
    int fd = open(dev, O_RDWR);
    if (fd < 0) { printf("open %s: %s\n", dev, strerror(errno)); return 1; }
    int idx = 0;
    int r = ioctl(fd, IIO_BUFFER_GET_FD_IOCTL, &idx);
    if (r < 0) { printf("GET_FD: %s\n", strerror(errno)); return 1; }
    int bfd = idx;
    int bad = -1;
    r = ioctl(bfd, IIO_BUFFER_DMABUF_ATTACH_IOCTL, &bad);
    printf("buffer fd %d; DMABUF_ATTACH(-1) -> %d errno=%d (%s) => %s\n", bfd, r, errno, strerror(errno),
           errno == EBADF ? "IIO DMABUF SUPPORTED" : "not supported");
    return 0;
}
