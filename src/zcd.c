// zcd: zero-copy RX/TX daemon for the PlutoSDR Nano (Tezuka + DMABUF kernel).
// One FunctionFS interface: bulk IN (RX), bulk OUT (TX), vendor control requests on ep0 (see zcproto.h).
// Sample data never touches the CPU: the same DMABUF is moved between the AD9361 DMA (IIO) and the USB UDC (FunctionFS).
//
// TX safety lives here, not in the host: disarmed at start/exit/disconnect, explicit ARM checked against an LO
// allow-list and an attenuation ceiling, watchdog disarm when the host stops feeding TX.
//
// usage: zcd FFS_DIR
#define _GNU_SOURCE
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include "zcproto.h"

// ---- kernel UAPI ----
struct iio_dmabuf { uint32_t fd; uint32_t flags; uint64_t bytes_used; };
#define IIO_BUFFER_GET_FD_IOCTL         _IOWR('i', 0x91, int)
#define IIO_BUFFER_DMABUF_ATTACH_IOCTL  _IOW('i', 0x92, int)
#define IIO_BUFFER_DMABUF_DETACH_IOCTL  _IOW('i', 0x93, int)
#define IIO_BUFFER_DMABUF_ENQUEUE_IOCTL _IOW('i', 0x94, struct iio_dmabuf)
struct usb_ffs_dmabuf_transfer_req { int fd; uint32_t flags; uint64_t length; } __attribute__((packed));
#define FUNCTIONFS_DMABUF_ATTACH   _IOW('g', 131, int)
#define FUNCTIONFS_DMABUF_DETACH   _IOW('g', 132, int)
#define FUNCTIONFS_DMABUF_TRANSFER _IOW('g', 133, struct usb_ffs_dmabuf_transfer_req)
struct dma_heap_allocation_data { uint64_t len; uint32_t fd; uint32_t fd_flags; uint64_t heap_flags; };
#define DMA_HEAP_IOCTL_ALLOC _IOWR('H', 0x0, struct dma_heap_allocation_data)
struct usb_ctrlrequest { uint8_t bRequestType, bRequest; uint16_t wValue, wIndex, wLength; } __attribute__((packed));
struct ffs_event { struct usb_ctrlrequest setup; uint8_t type; uint8_t pad[3]; } __attribute__((packed));
enum { FFS_BIND, FFS_UNBIND, FFS_ENABLE, FFS_DISABLE, FFS_SETUP, FFS_SUSPEND, FFS_RESUME };

// ---- descriptors: 1 interface, bulk IN 0x81 + bulk OUT 0x02 ----
struct __attribute__((packed)) intf_desc { uint8_t bLength, bDescriptorType, bInterfaceNumber, bAlternateSetting, bNumEndpoints, bInterfaceClass, bInterfaceSubClass, bInterfaceProtocol, iInterface; };
struct __attribute__((packed)) ep_desc { uint8_t bLength, bDescriptorType, bEndpointAddress, bmAttributes; uint16_t wMaxPacketSize; uint8_t bInterval; };
static const struct __attribute__((packed)) {
    struct { uint32_t magic, length, flags, fs_count, hs_count; } hdr;
    struct __attribute__((packed)) { struct intf_desc intf; struct ep_desc in, out; } fs, hs;
} descs = {
    .hdr = { htole32(3), htole32(sizeof(descs)), htole32(1 | 2), htole32(3), htole32(3) },
    .fs = { { 9, 4, 0, 0, 2, ZC_IF_CLASS, ZC_IF_SUBCLASS, ZC_IF_PROTOCOL, 1 }, { 7, 5, 0x81, 2, htole16(64), 0 }, { 7, 5, 0x02, 2, htole16(64), 0 } },
    .hs = { { 9, 4, 0, 0, 2, ZC_IF_CLASS, ZC_IF_SUBCLASS, ZC_IF_PROTOCOL, 1 }, { 7, 5, 0x81, 2, htole16(512), 0 }, { 7, 5, 0x02, 2, htole16(512), 0 } },
};
static const struct __attribute__((packed)) {
    struct { uint32_t magic, length, str_count, lang_count; } hdr;
    struct __attribute__((packed)) { uint16_t code; char s[17]; } lang0;
} strs = { { htole32(2), htole32(sizeof(strs)), htole32(1), htole32(1) }, { htole16(0x0409), "nano zero-copy 2" } };

// ---- state ----
#define PHY "/sys/bus/iio/devices/iio:device0"
#define RXD "/sys/bus/iio/devices/iio:device3"
#define TXD "/sys/bus/iio/devices/iio:device2"
#define ADC_CORE 0x79020000UL /* cf-ad9361-lpc, DT reg <0x79020000 0x6000> */
#define DAC_CORE 0x79024000UL /* cf-ad9361-dds-core-lpc, DT reg <0x79024000 0x1000> */

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int64_t P[ZC_P_MAX];
static struct zc_status st;
static volatile int rx_want, tx_want, rx_restart, tx_restart, quit;
static volatile double tx_last_feed;
static int ep0, ep_in, ep_out, heap;
static volatile uint32_t *adc, *dac;
static volatile short last_revents;
static int unlocked; // -u: owner-requested, no TX safeties (no arm step, limits, watchdog or auto-disarm)

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static void logf_(const char *f, ...) { va_list a; va_start(a, f); fprintf(stderr, "[%.3f] ", now()); vfprintf(stderr, f, a); fputc('\n', stderr); va_end(a); }

static int wsys(const char *path, const char *fmt, ...) {
    char v[96]; va_list a; va_start(a, fmt); vsnprintf(v, sizeof v, fmt, a); va_end(a);
    int fd = open(path, O_WRONLY);
    if (fd < 0) return -errno;
    int r = write(fd, v, strlen(v)) < 0 ? -errno : 0;
    close(fd);
    if (r) logf_("write %s=%s: %s", path, v, strerror(-r));
    return r;
}
static int rsys(const char *path, char *buf, size_t n) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -errno;
    int r = read(fd, buf, n - 1);
    close(fd);
    if (r < 0) return -errno;
    buf[r] = 0;
    return 0;
}
static int64_t rsys_ll(const char *path, double scale) { char b[96]; if (rsys(path, b, sizeof b)) return INT64_MIN; return (int64_t)(strtod(b, NULL) * scale); }

// ---- TX safety ----
static void disarm_force(const char *why);
static void disarm_locked(const char *why) {
    if (unlocked && strcmp(why, "host request") != 0) return; // unlocked: only an explicit TX-off disarms
    disarm_force(why);
}
static void disarm_force(const char *why) {
    wsys(PHY "/out_voltage0_hardwaregain", "-89.75");
    wsys(PHY "/out_altvoltage1_TX_LO_powerdown", "1");
    if (st.flags & ZC_F_TX_ARMED) logf_("TX disarmed (%s)", why);
    st.flags &= ~ZC_F_TX_ARMED;
}
static int tx_params_ok(int64_t lo, int64_t atten) {
    if (unlocked) return atten <= 0 && atten >= -89750; // only the chip's own range
    return lo >= P[ZC_P_TX_ALLOW_MIN_HZ] && lo <= P[ZC_P_TX_ALLOW_MAX_HZ] && atten <= 0 && atten >= -89750 && atten <= P[ZC_P_TX_ATTEN_CEIL_MDB];
}
static int arm_locked(void) {
    if (!tx_params_ok(P[ZC_P_TX_LO_HZ], P[ZC_P_TX_ATTEN_MDB])) return -EPERM;
    int r = wsys(PHY "/out_altvoltage1_TX_LO_frequency", "%lld", (long long)P[ZC_P_TX_LO_HZ]);
    if (!r) r = wsys(PHY "/out_voltage0_hardwaregain", "%.3f", P[ZC_P_TX_ATTEN_MDB] / 1000.0);
    if (!r) r = wsys(PHY "/out_altvoltage1_TX_LO_powerdown", "0");
    if (r) { disarm_locked("arm failed"); return r; }
    st.flags = (st.flags | ZC_F_TX_ARMED) & ~ZC_F_TX_WD;
    tx_last_feed = now();
    logf_("TX ARMED: LO %lld Hz, atten %.3f dB", (long long)P[ZC_P_TX_LO_HZ], P[ZC_P_TX_ATTEN_MDB] / 1000.0);
    return 0;
}

// ---- parameters ----
static const char *gain_modes[] = { "manual", "slow_attack", "fast_attack", "hybrid" };
static const char *rx_ports[] = { "A_BALANCED", "B_BALANCED", "C_BALANCED" };
static const char *tx_ports[] = { "A", "B" };

static int set_param_locked(int id, int64_t v) {
    switch (id) {
    case ZC_P_RX_LO_HZ: return wsys(PHY "/out_altvoltage0_RX_LO_frequency", "%lld", (long long)v) ?: (P[id] = v, 0);
    case ZC_P_TX_LO_HZ:
        if ((st.flags & ZC_F_TX_ARMED) && !tx_params_ok(v, P[ZC_P_TX_ATTEN_MDB])) return -EPERM;
        return wsys(PHY "/out_altvoltage1_TX_LO_frequency", "%lld", (long long)v) ?: (P[id] = v, 0);
    case ZC_P_RATE_HZ: return wsys(PHY "/in_voltage_sampling_frequency", "%lld", (long long)v) ?: (P[id] = v, st.rate_hz = v, 0);
    case ZC_P_RX_BW_HZ: return wsys(PHY "/in_voltage_rf_bandwidth", "%lld", (long long)v) ?: (P[id] = v, 0);
    case ZC_P_TX_BW_HZ: return wsys(PHY "/out_voltage_rf_bandwidth", "%lld", (long long)v) ?: (P[id] = v, 0);
    case ZC_P_RX_GAIN_MODE:
        if (v < 0 || v > 3) return -EINVAL;
        return wsys(PHY "/in_voltage0_gain_control_mode", "%s", gain_modes[v]) ?: (P[id] = v, 0);
    case ZC_P_RX_GAIN_MDB: return wsys(PHY "/in_voltage0_hardwaregain", "%.3f", v / 1000.0) ?: (P[id] = v, 0);
    case ZC_P_TX_ATTEN_MDB:
        if (v > 0 || v < -89750) return -EINVAL;
        if (st.flags & ZC_F_TX_ARMED) {
            if (!tx_params_ok(P[ZC_P_TX_LO_HZ], v)) return -EPERM;
            int r = wsys(PHY "/out_voltage0_hardwaregain", "%.3f", v / 1000.0);
            if (r) return r;
        }
        P[id] = v; return 0;
    case ZC_P_RX_BITS: case ZC_P_TX_BITS:
        if (v != 8 && v != 16) return -EINVAL;
        P[id] = v; if (id == ZC_P_RX_BITS) rx_restart = 1; else tx_restart = 1; return 0;
    case ZC_P_RX_NBUF: case ZC_P_TX_NBUF:
        if (v < 2 || v > 128) return -EINVAL;
        P[id] = v; if (id == ZC_P_RX_NBUF) rx_restart = 1; else tx_restart = 1; return 0;
    case ZC_P_RX_BUF_KIB: case ZC_P_TX_BUF_KIB:
        if (v < 16 || v > 8192) return -EINVAL;
        P[id] = v; if (id == ZC_P_RX_BUF_KIB) rx_restart = 1; else tx_restart = 1; return 0;
    case ZC_P_TX_WD_MS: if (v < 10 || v > 60000) return -EINVAL; P[id] = v; return 0;
    case ZC_P_TX_ALLOW_MIN_HZ: case ZC_P_TX_ALLOW_MAX_HZ:
        if (v < P[ZC_P_LO_MIN_HZ] || v > P[ZC_P_LO_MAX_HZ]) return -EINVAL;
        P[id] = v;
        if ((st.flags & ZC_F_TX_ARMED) && !tx_params_ok(P[ZC_P_TX_LO_HZ], P[ZC_P_TX_ATTEN_MDB])) disarm_locked("allow-list changed");
        return 0;
    case ZC_P_TX_ATTEN_CEIL_MDB:
        if (v > 0 || v < -89750) return -EINVAL;
        P[id] = v;
        if ((st.flags & ZC_F_TX_ARMED) && !tx_params_ok(P[ZC_P_TX_LO_HZ], P[ZC_P_TX_ATTEN_MDB])) disarm_locked("ceiling changed");
        return 0;
    case ZC_P_RX_PORT: if (v < 0 || v > 2) return -EINVAL; return wsys(PHY "/in_voltage0_rf_port_select", "%s", rx_ports[v]) ?: (P[id] = v, 0);
    case ZC_P_TX_PORT: if (v < 0 || v > 1) return -EINVAL; return wsys(PHY "/out_voltage0_rf_port_select", "%s", tx_ports[v]) ?: (P[id] = v, 0);
    case ZC_P_RX_QUAD_TRACK: return wsys(PHY "/in_voltage_quadrature_tracking_en", "%d", !!v) ?: (P[id] = !!v, 0);
    case ZC_P_RX_RF_DC_TRACK: return wsys(PHY "/in_voltage_rf_dc_offset_tracking_en", "%d", !!v) ?: (P[id] = !!v, 0);
    case ZC_P_RX_BB_DC_TRACK: return wsys(PHY "/in_voltage_bb_dc_offset_tracking_en", "%d", !!v) ?: (P[id] = !!v, 0);
    default: return -EINVAL;
    }
}

static int64_t get_param_locked(int id) {
    switch (id) {
    case ZC_P_RX_LO_HZ: return rsys_ll(PHY "/out_altvoltage0_RX_LO_frequency", 1);
    case ZC_P_TX_LO_HZ: return rsys_ll(PHY "/out_altvoltage1_TX_LO_frequency", 1);
    case ZC_P_RATE_HZ: return rsys_ll(PHY "/in_voltage_sampling_frequency", 1);
    case ZC_P_RX_BW_HZ: return rsys_ll(PHY "/in_voltage_rf_bandwidth", 1);
    case ZC_P_TX_BW_HZ: return rsys_ll(PHY "/out_voltage_rf_bandwidth", 1);
    case ZC_P_RX_GAIN_MDB: return rsys_ll(PHY "/in_voltage0_hardwaregain", 1000);
    case ZC_P_TX_ATTEN_MDB: return P[id]; // requested value (the chip sits at -89.75 dB while disarmed)
    case ZC_P_RX_RSSI_MDB: return rsys_ll(PHY "/in_voltage0_rssi", 1000);
    case ZC_P_TEMP_MC: return rsys_ll(PHY "/in_temp0_input", 1);
    case ZC_P_ZYNQ_TEMP_MC: {
        int64_t raw = rsys_ll("/sys/bus/iio/devices/iio:device1/in_temp0_raw", 1), off = rsys_ll("/sys/bus/iio/devices/iio:device1/in_temp0_offset", 1);
        char b[64]; if (raw == INT64_MIN || off == INT64_MIN || rsys("/sys/bus/iio/devices/iio:device1/in_temp0_scale", b, sizeof b)) return INT64_MIN;
        return (int64_t)((raw + off) * strtod(b, NULL));
    }
    default: return (id > 0 && id < ZC_P_MAX) ? P[id] : INT64_MIN;
    }
}

// ---- DMABUF ring helpers ----
struct ring { int n; size_t len; int *fd; int *state; };
static int ring_alloc(struct ring *r, int n, size_t len, int iiobfd, int epfd) {
    r->n = n; r->len = len; r->fd = calloc(n, sizeof(int)); r->state = calloc(n, sizeof(int));
    for (int i = 0; i < n; i++) {
        struct dma_heap_allocation_data a = { .len = len, .fd_flags = O_RDWR | O_CLOEXEC };
        if (ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &a) < 0) return -errno;
        r->fd[i] = a.fd;
        if (ioctl(iiobfd, IIO_BUFFER_DMABUF_ATTACH_IOCTL, &r->fd[i]) < 0) return -errno;
        if (ioctl(epfd, FUNCTIONFS_DMABUF_ATTACH, &r->fd[i]) < 0) return -errno;
    }
    return 0;
}
static void ring_free(struct ring *r, int iiobfd, int epfd) {
    for (int i = 0; i < r->n; i++) {
        if (r->fd[i] <= 0) continue;
        ioctl(epfd, FUNCTIONFS_DMABUF_DETACH, &r->fd[i]);   // cancels a pending USB transfer
        ioctl(iiobfd, IIO_BUFFER_DMABUF_DETACH_IOCTL, &r->fd[i]);
        close(r->fd[i]);
    }
    free(r->fd); free(r->state); memset(r, 0, sizeof *r);
}
static volatile uint32_t *rxdmac, *txdmac;
static void dump_dmac(const char *why) {
    static const int off[] = { 0x080, 0x084, 0x088, 0x400, 0x404, 0x408, 0x40c, 0x410, 0x418, 0x428, 0x42c, 0x430, 0x434 };
    char b[512]; int n;
    for (int k = 0; k < 2; k++) {
        volatile uint32_t *d = k ? txdmac : rxdmac;
        if (!d) continue;
        n = snprintf(b, sizeof b, "%s %s-DMAC:", why, k ? "TX" : "RX");
        for (unsigned i = 0; i < sizeof off / sizeof off[0]; i++) n += snprintf(b + n, sizeof b - n, " %03x=%08x", off[i], d[off[i] / 4]);
        logf_("%s", b);
    }
}
static volatile unsigned long enq_busy_retries, enq_busy_events;
static int iio_enqueue(int bfd, int fd, size_t len) {
    struct iio_dmabuf d = { .fd = fd, .bytes_used = len };
    for (int tries = 0;; tries++) {
        if (ioctl(bfd, IIO_BUFFER_DMABUF_ENQUEUE_IOCTL, &d) == 0) {
            if (tries) { enq_busy_events++; enq_busy_retries += tries; if (enq_busy_events <= 20 || enq_busy_events % 1000 == 0) logf_("enqueue EBUSY cleared after %d retries (events %lu)", tries, enq_busy_events); }
            return 0;
        }
        if (errno != EBUSY || tries >= 40) {
            int e = errno;
            if (e == EBUSY) { logf_("enqueue EBUSY persisted after %d retries (last poll revents 0x%x)", tries, last_revents); dump_dmac("EBUSY"); }
            return -e;
        }
        usleep(500);
    }
}
static int usb_xfer(int ep, int fd, size_t len) { struct usb_ffs_dmabuf_transfer_req t = { .fd = fd, .length = len }; return ioctl(ep, FUNCTIONFS_DMABUF_TRANSFER, &t) < 0 ? -errno : 0; }
static int ready(int fd, short ev) { struct pollfd p = { .fd = fd, .events = ev }; int r = poll(&p, 1, 0); last_revents = p.revents; return r == 1 && (p.revents & ev); }
static void wait_any(int a, short ea, int b, short eb, int ms) { struct pollfd p[2] = { { .fd = a, .events = ea }, { .fd = b, .events = eb } }; poll(p, b >= 0 ? 2 : 1, ms); }

// ---- RX: ADC DMA fills -> USB IN ----
static void *rx_thread(void *u) {
    (void)u;
    for (;;) {
        pthread_mutex_lock(&mu);
        while (!rx_want && !quit) pthread_cond_wait(&cv, &mu);
        if (quit) { pthread_mutex_unlock(&mu); return NULL; }
        int bits = P[ZC_P_RX_BITS], nbuf = P[ZC_P_RX_NBUF]; size_t len = (size_t)P[ZC_P_RX_BUF_KIB] * 1024;
        rx_restart = 0;
        wsys(RXD "/buffer0/enable", "0");
        wsys(RXD "/scan_elements/in_voltage0_en", "1");
        wsys(RXD "/scan_elements/in_voltage1_en", bits == 16 ? "1" : "0");
        pthread_mutex_unlock(&mu);

        int dev = open("/dev/iio:device3", O_RDWR), bfd = 0;
        struct ring r = { 0 };
        int err = dev < 0 || ioctl(dev, IIO_BUFFER_GET_FD_IOCTL, &bfd) < 0 ? -errno : 0;
        if (!err) err = ring_alloc(&r, nbuf, len, bfd, ep_in);
        for (int i = 0; !err && i < nbuf; i++) { err = iio_enqueue(bfd, r.fd[i], len); r.state[i] = 1; }
        if (!err) err = wsys(RXD "/buffer0/enable", "1");
        if (adc) adc[0x88 / 4] = 4;
        pthread_mutex_lock(&mu); st.rx_bytes = 0; st.rx_ovf = 0; st.rx_bits = bits; if (!err) st.flags |= ZC_F_RX_RUN; pthread_mutex_unlock(&mu);
        logf_("RX start: %d-bit, %d x %zu KiB: %s", bits, nbuf, len / 1024, err ? strerror(-err) : "ok");

        int head = 0; double tpoll = now();
        while (!err && rx_want && !rx_restart && !quit) {
            int j = head;
            if (r.state[j] == 1 && ready(r.fd[j], POLLIN)) {            // ADC filled -> USB
                if ((err = usb_xfer(ep_in, r.fd[j], len))) { logf_("RX usb_xfer buf %d: %s", j, strerror(-err)); break; }
                r.state[j] = 2;
                for (int k = 1; k < nbuf; k++) {                          // keep USB fed
                    int q = (head + k) % nbuf;
                    if (r.state[q] != 1 || !ready(r.fd[q], POLLIN)) break;
                    if (usb_xfer(ep_in, r.fd[q], len)) break;
                    r.state[q] = 2;
                }
            } else if (r.state[j] == 2 && ready(r.fd[j], POLLOUT)) {     // USB sent -> back to ADC
                if ((err = iio_enqueue(bfd, r.fd[j], len))) { logf_("RX iio_enqueue buf %d: %s", j, strerror(-err)); break; }
                r.state[j] = 1; head = (head + 1) % nbuf;
                pthread_mutex_lock(&mu); st.rx_bytes += len; pthread_mutex_unlock(&mu);
            } else {
                wait_any(r.fd[j], r.state[j] == 1 ? POLLIN : POLLOUT, -1, 0, 50);
            }
            if (adc && now() - tpoll > 0.1) {
                tpoll = now();
                if (adc[0x88 / 4] & 4) { adc[0x88 / 4] = 4; pthread_mutex_lock(&mu); st.rx_ovf++; pthread_mutex_unlock(&mu); }
            }
        }
        wsys(RXD "/buffer0/enable", "0");
        if (r.n) ring_free(&r, bfd, ep_in);
        if (bfd > 0) close(bfd);
        if (dev >= 0) close(dev);
        pthread_mutex_lock(&mu);
        st.flags &= ~ZC_F_RX_RUN;
        if (err) { st.last_err = -err; rx_want = 0; }
        pthread_mutex_unlock(&mu);
        logf_("RX stop%s%s", err ? ": " : "", err ? strerror(-err) : "");
    }
}

// ---- TX: USB OUT fills -> DAC DMA ----
static void *tx_thread(void *u) {
    (void)u;
    for (;;) {
        pthread_mutex_lock(&mu);
        while (!tx_want && !quit) pthread_cond_wait(&cv, &mu);
        if (quit) { pthread_mutex_unlock(&mu); return NULL; }
        int bits = P[ZC_P_TX_BITS], nbuf = P[ZC_P_TX_NBUF]; size_t len = (size_t)P[ZC_P_TX_BUF_KIB] * 1024;
        tx_restart = 0;
        wsys(TXD "/buffer0/enable", "0");
        wsys(TXD "/scan_elements/out_voltage0_en", "1");
        wsys(TXD "/scan_elements/out_voltage1_en", bits == 16 ? "1" : "0");
        pthread_mutex_unlock(&mu);

        int dev = open("/dev/iio:device2", O_RDWR), bfd = 0;
        struct ring r = { 0 };
        int err = dev < 0 || ioctl(dev, IIO_BUFFER_GET_FD_IOCTL, &bfd) < 0 ? -errno : 0;
        if (!err) err = ring_alloc(&r, nbuf, len, bfd, ep_out);
        for (int i = 0; !err && i < nbuf; i++) { err = usb_xfer(ep_out, r.fd[i], len); r.state[i] = 1; } // all to USB first
        pthread_mutex_lock(&mu); st.tx_bytes = 0; st.tx_unf = 0; st.tx_bits = bits; if (!err) st.flags |= ZC_F_TX_RUN; pthread_mutex_unlock(&mu);
        logf_("TX start: %d-bit, %d x %zu KiB: %s", bits, nbuf, len / 1024, err ? strerror(-err) : "ok");
        if (!err && unlocked) { pthread_mutex_lock(&mu); if (!(st.flags & ZC_F_TX_ARMED)) arm_locked(); pthread_mutex_unlock(&mu); }

        int uh = 0, ih = 0, queued = 0, enabled = 0, preroll = nbuf / 2 > 0 ? nbuf / 2 : 1;
        double tpoll = now();
        tx_last_feed = now();
        while (!err && tx_want && !tx_restart && !quit) {
            int progressed = 0;
            if (r.state[uh] == 1 && ready(r.fd[uh], POLLIN)) {          // host filled -> DAC
                if ((err = iio_enqueue(bfd, r.fd[uh], len))) { logf_("TX iio_enqueue buf %d: %s", uh, strerror(-err)); break; }
                r.state[uh] = 2; uh = (uh + 1) % nbuf; queued++; progressed = 1; tx_last_feed = now();
                if (!enabled && queued >= preroll) {
                    if (dac) dac[0x88 / 4] = 2;
                    if ((err = wsys(TXD "/buffer0/enable", "1"))) { logf_("TX enable: %s", strerror(-err)); break; }
                    enabled = 1;
                }
            }
            if (enabled && r.state[ih] == 2 && ready(r.fd[ih], POLLOUT)) { // DAC done -> back to host
                if ((err = usb_xfer(ep_out, r.fd[ih], len))) { logf_("TX usb_xfer buf %d: %s", ih, strerror(-err)); break; }
                r.state[ih] = 1; ih = (ih + 1) % nbuf; progressed = 1;
                pthread_mutex_lock(&mu); st.tx_bytes += len; pthread_mutex_unlock(&mu);
            }
            if (!progressed) wait_any(r.fd[uh], POLLIN, enabled ? r.fd[ih] : -1, POLLOUT, 20);
            double t = now();
            pthread_mutex_lock(&mu);
            if (!unlocked && (st.flags & ZC_F_TX_ARMED) && t - tx_last_feed > P[ZC_P_TX_WD_MS] / 1000.0) { disarm_locked("watchdog: host stopped feeding TX"); st.flags |= ZC_F_TX_WD; }
            pthread_mutex_unlock(&mu);
            if (dac && enabled && t - tpoll > 0.1) {
                tpoll = t;
                if (dac[0x88 / 4] & 2) { dac[0x88 / 4] = 2; pthread_mutex_lock(&mu); st.tx_unf++; pthread_mutex_unlock(&mu); }
            }
        }
        pthread_mutex_lock(&mu); disarm_locked("TX stream stopped"); pthread_mutex_unlock(&mu);
        wsys(TXD "/buffer0/enable", "0");
        if (r.n) ring_free(&r, bfd, ep_out);
        if (bfd > 0) close(bfd);
        if (dev >= 0) close(dev);
        pthread_mutex_lock(&mu);
        st.flags &= ~ZC_F_TX_RUN;
        if (err) { st.last_err = -err; tx_want = 0; }
        pthread_mutex_unlock(&mu);
        logf_("TX stop%s%s", err ? ": " : "", err ? strerror(-err) : "");
    }
}

// ---- control plane on ep0 ----
static void stall(const struct usb_ctrlrequest *s) { if (s->bRequestType & 0x80) (void)!read(ep0, NULL, 0); else (void)!write(ep0, NULL, 0); }

static void handle_setup(const struct usb_ctrlrequest *s) {
    uint16_t len = le16toh(s->wLength), val = le16toh(s->wValue);
    if ((s->bRequestType & 0x60) != 0x40) { stall(s); return; } // vendor only
    int in = s->bRequestType & 0x80, err = 0;
    pthread_mutex_lock(&mu);
    if (!in) st.last_err = 0; // errors are reported per request, never masked by an older one
    if (in) {
        if (s->bRequest == ZC_REQ_GET && len >= 8) {
            int64_t v = get_param_locked(val);
            pthread_mutex_unlock(&mu);
            if (v == INT64_MIN) { stall(s); return; }
            uint64_t le = htole64((uint64_t)v);
            (void)!write(ep0, &le, 8);
            return;
        }
        if (s->bRequest == ZC_REQ_STATUS && len >= sizeof st) {
            st.temp_mc = (int32_t)get_param_locked(ZC_P_TEMP_MC);
            struct zc_status c = st;
            pthread_mutex_unlock(&mu);
            (void)!write(ep0, &c, sizeof c);
            return;
        }
        pthread_mutex_unlock(&mu); stall(s); return;
    }
    // host -> device
    int64_t v = 0;
    if (len) {
        pthread_mutex_unlock(&mu);
        uint64_t le = 0;
        if (len != 8 || read(ep0, &le, 8) != 8) { return; }
        v = (int64_t)le64toh(le);
        pthread_mutex_lock(&mu);
    }
    switch (s->bRequest) {
    case ZC_REQ_SET: err = set_param_locked(val, v); if (err) logf_("SET %u=%lld: %s", val, (long long)v, strerror(-err)); break;
    case ZC_REQ_STREAM:
        rx_want = val & 1; tx_want = (val >> 1) & 1;
        if (!tx_want) disarm_locked("TX stream off");
        pthread_cond_broadcast(&cv);
        break;
    case ZC_REQ_ARM: err = val ? arm_locked() : (disarm_locked("host request"), 0); break;
    default: err = -EINVAL;
    }
    if (err) st.last_err = -err;
    pthread_mutex_unlock(&mu);
    if (len) return;                                // data stage already acked the request
    if (err) stall(s); else (void)!read(ep0, NULL, 0); // status stage
}

static void on_sig(int s) { (void)s; quit = 1; }

int main(int argc, char **argv) {
    if (argc > 2 && !strcmp(argv[1], "-u")) { unlocked = 1; argv++; argc--; }
    if (argc < 2) { fprintf(stderr, "usage: %s [-u] FFS_DIR\n", argv[0]); return 2; }
    char p[256];
    signal(SIGINT, on_sig); signal(SIGTERM, on_sig); signal(SIGPIPE, SIG_IGN);

    // defaults; TX disarmed before anything else
    P[ZC_P_LO_MIN_HZ] = 46875001; P[ZC_P_LO_MAX_HZ] = 6000000000LL;
    { char b[96]; if (!rsys(PHY "/out_altvoltage1_TX_LO_frequency_available", b, sizeof b)) { long long a, s2, c; if (sscanf(b, "[%lld %lld %lld]", &a, &s2, &c) == 3) { P[ZC_P_LO_MIN_HZ] = a; P[ZC_P_LO_MAX_HZ] = c; } } }
    P[ZC_P_TX_ALLOW_MIN_HZ] = P[ZC_P_LO_MIN_HZ]; P[ZC_P_TX_ALLOW_MAX_HZ] = P[ZC_P_LO_MAX_HZ]; // owner: full range (chamber)
    P[ZC_P_TX_ATTEN_CEIL_MDB] = 0; P[ZC_P_TX_ATTEN_MDB] = -89750; P[ZC_P_TX_WD_MS] = 250;
    P[ZC_P_RX_BITS] = 8; P[ZC_P_TX_BITS] = 8;
    P[ZC_P_RX_NBUF] = 32; P[ZC_P_RX_BUF_KIB] = 64; P[ZC_P_TX_NBUF] = 32; P[ZC_P_TX_BUF_KIB] = 64;
    P[ZC_P_TX_LO_HZ] = get_param_locked(ZC_P_TX_LO_HZ); P[ZC_P_RX_LO_HZ] = get_param_locked(ZC_P_RX_LO_HZ);
    P[ZC_P_RATE_HZ] = st.rate_hz = get_param_locked(ZC_P_RATE_HZ);
    disarm_force("startup");

    heap = open("/dev/dma_heap/reserved", O_RDWR);
    if (heap < 0) heap = open("/dev/dma_heap/linux,cma", O_RDWR);
    if (heap < 0) { perror("dma_heap"); return 1; }
    int mfd = open("/dev/mem", O_RDWR | O_SYNC);
    if (mfd >= 0) {
        void *a = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, ADC_CORE), *d = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, DAC_CORE);
        if (a != MAP_FAILED) adc = a;
        if (d != MAP_FAILED) dac = d;
        void *rd = mmap(NULL, 4096, PROT_READ, MAP_SHARED, mfd, 0x7c400000), *td = mmap(NULL, 4096, PROT_READ, MAP_SHARED, mfd, 0x7c420000); // DT rx_dma/tx_dma, read-only
        if (rd != MAP_FAILED) rxdmac = rd;
        if (td != MAP_FAILED) txdmac = td;
    }

    snprintf(p, sizeof p, "%s/ep0", argv[1]);
    ep0 = open(p, O_RDWR);
    if (ep0 < 0 || write(ep0, &descs, sizeof descs) < 0 || write(ep0, &strs, sizeof strs) < 0) { perror("ffs ep0"); return 1; }
    logf_("ffs descriptors written; bind the UDC now%s", unlocked ? " [UNLOCKED: no TX safeties]" : "");
    snprintf(p, sizeof p, "%s/ep1", argv[1]); ep_in = open(p, O_RDWR);
    snprintf(p, sizeof p, "%s/ep2", argv[1]); ep_out = open(p, O_RDWR);
    if (ep_in < 0 || ep_out < 0) { perror("ffs ep1/ep2"); return 1; }

    pthread_t trx, ttx;
    pthread_create(&trx, NULL, rx_thread, NULL);
    pthread_create(&ttx, NULL, tx_thread, NULL);

    while (!quit) {
        struct ffs_event ev[4];
        int n = read(ep0, ev, sizeof ev);
        if (n < 0) { if (errno == EINTR) continue; perror("ep0 read"); break; }
        for (int i = 0; i < n / (int)sizeof ev[0]; i++) {
            switch (ev[i].type) {
            case FFS_SETUP: handle_setup(&ev[i].setup); break;
            case FFS_ENABLE: logf_("USB enabled"); break;
            case FFS_DISABLE: case FFS_UNBIND: case FFS_SUSPEND:
                pthread_mutex_lock(&mu);
                rx_want = tx_want = 0; disarm_locked(ev[i].type == FFS_SUSPEND ? "USB suspend" : "USB disconnect");
                pthread_cond_broadcast(&cv); pthread_mutex_unlock(&mu);
                logf_("USB event %d: streams stopped", ev[i].type);
                break;
            default: break;
            }
        }
    }
    pthread_mutex_lock(&mu); quit = 1; rx_want = tx_want = 0; disarm_locked("exit"); pthread_cond_broadcast(&cv); pthread_mutex_unlock(&mu);
    pthread_join(trx, NULL); pthread_join(ttx, NULL);
    return 0;
}
