// zcstream: zero-copy IIO RX -> USB (FunctionFS) streamer using DMABUFs.
//
// The same DMABUF is filled by the ADC DMA (IIO DMABUF API) and sent by the USB UDC DMA
// (FunctionFS DMABUF API); the CPU never touches sample data.
//
// usage: zcstream FFS_DIR RATE_HZ cs8|cs16 [NBUF] [BUF_KIB] [ADC_CORE_PHYS]
//   FFS_DIR       mounted functionfs instance (ep0 inside); descriptors are written here
//   ADC_CORE_PHYS physical base of cf-ad9361-lpc (from the device tree) for the DMA_OVF bit
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <endian.h>

// ---- UAPI (from include/uapi/linux/{iio/buffer.h,usb/functionfs.h,dma-heap.h}) ----
struct iio_dmabuf { uint32_t fd; uint32_t flags; uint64_t bytes_used; };
#define IIO_BUFFER_GET_FD_IOCTL         _IOWR('i', 0x91, int)
#define IIO_BUFFER_DMABUF_ATTACH_IOCTL  _IOW('i', 0x92, int)
#define IIO_BUFFER_DMABUF_ENQUEUE_IOCTL _IOW('i', 0x94, struct iio_dmabuf)

struct usb_ffs_dmabuf_transfer_req { int fd; uint32_t flags; uint64_t length; } __attribute__((packed));
#define FUNCTIONFS_DMABUF_ATTACH   _IOW('g', 131, int)
#define FUNCTIONFS_DMABUF_TRANSFER _IOW('g', 133, struct usb_ffs_dmabuf_transfer_req)

struct dma_heap_allocation_data { uint64_t len; uint32_t fd; uint32_t fd_flags; uint64_t heap_flags; };
#define DMA_HEAP_IOCTL_ALLOC _IOWR('H', 0x0, struct dma_heap_allocation_data)

// ---- FunctionFS descriptors: 1 interface, vendor class, 1 bulk IN endpoint ----
#define FUNCTIONFS_DESCRIPTORS_MAGIC_V2 3
#define FUNCTIONFS_STRINGS_MAGIC 2
#define FUNCTIONFS_HAS_FS_DESC 1
#define FUNCTIONFS_HAS_HS_DESC 2
struct __attribute__((packed)) intf_desc { uint8_t bLength, bDescriptorType, bInterfaceNumber, bAlternateSetting, bNumEndpoints, bInterfaceClass, bInterfaceSubClass, bInterfaceProtocol, iInterface; };
struct __attribute__((packed)) ep_desc { uint8_t bLength, bDescriptorType, bEndpointAddress, bmAttributes; uint16_t wMaxPacketSize; uint8_t bInterval; };
static const struct __attribute__((packed)) {
    struct { uint32_t magic, length, flags, fs_count, hs_count; } hdr;
    struct __attribute__((packed)) { struct intf_desc intf; struct ep_desc in; } fs, hs;
} descs = {
    .hdr = { htole32(FUNCTIONFS_DESCRIPTORS_MAGIC_V2), htole32(sizeof(descs)),
             htole32(FUNCTIONFS_HAS_FS_DESC | FUNCTIONFS_HAS_HS_DESC), htole32(2), htole32(2) },
    .fs = { { 9, 4, 0, 0, 1, 0xff, 0x5a, 0x01, 1 }, { 7, 5, 0x81, 2, htole16(64), 0 } },
    .hs = { { 9, 4, 0, 0, 1, 0xff, 0x5a, 0x01, 1 }, { 7, 5, 0x81, 2, htole16(512), 0 } },
};
static const struct __attribute__((packed)) {
    struct { uint32_t magic, length, str_count, lang_count; } hdr;
    struct __attribute__((packed)) { uint16_t code; char s[15]; } lang0;
} strs = { { htole32(FUNCTIONFS_STRINGS_MAGIC), htole32(sizeof(strs)), htole32(1), htole32(1) }, { htole16(0x0409), "nano zero-copy" } };

static volatile int stop;
static void on_sig(int s) { (void)s; stop = 1; }

static int wfile(const char *path, const char *val) {
    int fd = open(path, O_WRONLY);
    if (fd < 0) { fprintf(stderr, "open %s: %s\n", path, strerror(errno)); return -1; }
    int r = write(fd, val, strlen(val));
    close(fd);
    if (r < 0) { fprintf(stderr, "write %s=%s: %s\n", path, val, strerror(errno)); return -1; }
    return 0;
}

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: %s FFS_DIR RATE_HZ cs8|cs16 [NBUF] [BUF_KIB] [ADC_CORE_PHYS]\n", argv[0]); return 2; }
    const char *ffs = argv[1];
    long rate = atol(argv[2]);
    int cs8 = !strcmp(argv[3], "cs8");
    int nbuf = argc > 4 ? atoi(argv[4]) : 8;
    size_t blen = (size_t)(argc > 5 ? atoi(argv[5]) : 1024) * 1024;
    unsigned long core = argc > 6 ? strtoul(argv[6], NULL, 0) : 0;
    const char *phy = "/sys/bus/iio/devices/iio:device0", *rx = "/sys/bus/iio/devices/iio:device3";
    char p[256], v[64];
    signal(SIGINT, on_sig); signal(SIGTERM, on_sig);

    // --- FunctionFS: write descriptors, open bulk IN endpoint ---
    snprintf(p, sizeof p, "%s/ep0", ffs);
    int ep0 = open(p, O_RDWR);
    if (ep0 < 0 || write(ep0, &descs, sizeof descs) < 0 || write(ep0, &strs, sizeof strs) < 0) { perror("ffs ep0"); return 1; }
    fprintf(stderr, "ffs descriptors written; bind the UDC now\n");
    snprintf(p, sizeof p, "%s/ep1", ffs);
    int ep1 = -1;
    for (int i = 0; i < 600 && ep1 < 0 && !stop; i++) { ep1 = open(p, O_RDWR); if (ep1 < 0) usleep(100000); }
    if (ep1 < 0) { perror("ffs ep1"); return 1; }
    // DMABUF attach needs a bound, enabled function (gadget pointer set): wait for FUNCTIONFS_ENABLE.
    for (int enabled = 0; !enabled && !stop;) {
        struct { uint8_t setup[8]; uint8_t type; uint8_t pad[3]; } ev[4];
        int n = read(ep0, ev, sizeof ev);
        if (n < 0) { if (errno == EINTR) continue; perror("ffs ep0 events"); return 1; }
        for (int i = 0; i < n / (int)sizeof ev[0]; i++) {
            fprintf(stderr, "ffs event %d\n", ev[i].type);
            if (ev[i].type == 2) enabled = 1; // FUNCTIONFS_ENABLE
        }
    }

    // --- IIO: rate, channels, buffer ---
    snprintf(v, sizeof v, "%ld", rate);
    snprintf(p, sizeof p, "%s/in_voltage_sampling_frequency", phy);
    if (wfile(p, v)) return 1;
    snprintf(p, sizeof p, "%s/buffer0/enable", rx); wfile(p, "0");
    snprintf(p, sizeof p, "%s/scan_elements/in_voltage0_en", rx); if (wfile(p, "1")) return 1;
    snprintf(p, sizeof p, "%s/scan_elements/in_voltage1_en", rx); if (wfile(p, cs8 ? "0" : "1")) return 1;
    int iiofd = open("/dev/iio:device3", O_RDWR);
    int bfd = 0;
    if (iiofd < 0 || ioctl(iiofd, IIO_BUFFER_GET_FD_IOCTL, &bfd) < 0) { perror("iio buffer fd"); return 1; }

    // --- DMABUFs from the CMA heap (physically contiguous for axi-dmac) ---
    int heap = open("/dev/dma_heap/linux,cma", O_RDWR);
    if (heap < 0) heap = open("/dev/dma_heap/reserved", O_RDWR);
    if (heap < 0) { perror("dma_heap"); return 1; }
    int *fds = calloc(nbuf, sizeof(int)), *state = calloc(nbuf, sizeof(int)); // state: 1=IIO, 2=USB
    for (int i = 0; i < nbuf; i++) {
        struct dma_heap_allocation_data a = { .len = blen, .fd_flags = O_RDWR | O_CLOEXEC };
        if (ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &a) < 0) { perror("heap alloc"); return 1; }
        fds[i] = a.fd;
        if (ioctl(bfd, IIO_BUFFER_DMABUF_ATTACH_IOCTL, &fds[i]) < 0) { perror("iio attach"); return 1; }
        if (ioctl(ep1, FUNCTIONFS_DMABUF_ATTACH, &fds[i]) < 0) { perror("ffs attach"); return 1; }
    }

    // --- ADC core DMA_OVF (reg 0x88 bit 2, W1C) via /dev/mem, page confirmed from the DT ---
    volatile uint32_t *regs = NULL;
    if (core) {
        int mfd = open("/dev/mem", O_RDWR | O_SYNC);
        void *m = mfd >= 0 ? mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, core) : MAP_FAILED;
        if (m != MAP_FAILED) regs = m; else perror("mmap adc core");
    }

    snprintf(p, sizeof p, "%s/buffer0/enable", rx);
    for (int i = 0; i < nbuf; i++) {
        struct iio_dmabuf d = { .fd = fds[i], .bytes_used = blen };
        if (ioctl(bfd, IIO_BUFFER_DMABUF_ENQUEUE_IOCTL, &d) < 0) { perror("enqueue"); return 1; }
        state[i] = 1;
    }
    if (wfile(p, "1")) return 1;
    if (regs) regs[0x88 / 4] = 4; // clear sticky overflow after start

    unsigned long long sent = 0, ovf_events = 0, loops = 0;
    double t0 = now(), tlast = t0;
    int head = 0; // buffers complete in order
    fprintf(stderr, "streaming %s @ %ld Hz, %d x %zu KiB dmabufs\n", cs8 ? "cs8" : "cs16", rate, nbuf, blen / 1024);
    while (!stop) {
        struct pollfd pf = { .fd = fds[head], .events = state[head] == 1 ? POLLIN : POLLOUT };
        int r = poll(&pf, 1, 2000);
        if (r <= 0) { if (r == 0) fprintf(stderr, "timeout waiting on buf %d state %d\n", head, state[head]); continue; }
        if (state[head] == 1) { // ADC filled it -> send over USB
            struct usb_ffs_dmabuf_transfer_req t = { .fd = fds[head], .length = blen };
            if (ioctl(ep1, FUNCTIONFS_DMABUF_TRANSFER, &t) < 0) { if (errno == EINTR) continue; perror("ffs transfer"); break; }
            state[head] = 2;
            // look ahead: the oldest USB buffer may be done -> back to IIO
        } else { // USB sent it -> back to the ADC
            struct iio_dmabuf d = { .fd = fds[head], .bytes_used = blen };
            if (ioctl(bfd, IIO_BUFFER_DMABUF_ENQUEUE_IOCTL, &d) < 0) { perror("re-enqueue"); break; }
            state[head] = 1;
            sent += blen;
            head = (head + 1) % nbuf;
        }
        // keep the pipeline full: launch USB for any later buffer the ADC has already finished
        for (int k = 1; k < nbuf; k++) {
            int j = (head + k) % nbuf;
            if (state[j] != 1) continue;
            struct pollfd q = { .fd = fds[j], .events = POLLIN };
            if (poll(&q, 1, 0) != 1) break;
            struct usb_ffs_dmabuf_transfer_req t = { .fd = fds[j], .length = blen };
            if (ioctl(ep1, FUNCTIONFS_DMABUF_TRANSFER, &t) < 0) break;
            state[j] = 2;
        }
        loops++;
        double tn = now();
        if (tn - tlast >= 1.0) {
            int ovf = regs ? (regs[0x88 / 4] & 4) != 0 : -1;
            if (ovf == 1) { ovf_events++; regs[0x88 / 4] = 4; }
            int bps = cs8 ? 2 : 4;
            fprintf(stderr, "t=%.0fs sent=%.1f MB rate=%.2f MB/s (%.3f MS/s) ovf_events=%llu\n",
                    tn - t0, sent / 1e6, sent / (tn - t0) / 1e6, sent / (tn - t0) / bps / 1e6, ovf_events);
            tlast = tn;
        }
    }
    wfile(p, "0");
    fprintf(stderr, "done: sent %.1f MB in %.1f s, ovf_events=%llu\n", sent / 1e6, now() - t0, ovf_events);
    return 0;
}
