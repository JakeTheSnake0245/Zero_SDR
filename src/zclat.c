// zclat: end-to-end latency of the zero-copy RX stream.
// Turns the AD9363 RX BIST tone (mode 2 = RX inject, never TX) on via the UART console, timestamps the
// command, and detects the tone (fs/8) in the incoming CS8 stream. Reports command->samples latency.
// usage: zclat RATE_HZ TRIALS [NXFER] [XFER_KIB]
#include <fcntl.h>
#include <glob.h>
#include <libusb-1.0/libusb.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

static int ser = -1;
static void console(const char *cmd) {
    write(ser, cmd, strlen(cmd));
    write(ser, "\r", 1);
    tcdrain(ser); // command fully on the wire
}

static volatile double t_seen;
static volatile int want_on; // 1: waiting for tone on, 0: waiting for tone off
static double thresh_on = 60.0, thresh_off = 15.0;
static int running = 1;

// mean |sum z e^{-j pi n/4}| / N over the last 4096 CS8 samples of a transfer
static double tone_level(const unsigned char *b, int len) {
    int n = 4096;
    if (len < 2 * n) n = len / 2;
    const signed char *s = (const signed char *)b + len - 2 * n;
    static const double c[8] = {1, M_SQRT1_2, 0, -M_SQRT1_2, -1, -M_SQRT1_2, 0, M_SQRT1_2};
    double re = 0, im = 0;
    for (int k = 0; k < n; k++) {
        double I = s[2 * k], Q = s[2 * k + 1];
        double cr = c[k & 7], ci = c[(k + 2) & 7]; // e^{-j pi k/4}: cos(pi k/4) - j sin(pi k/4), and -sin(x) = cos(x + pi/2)
        re += I * cr - Q * ci;
        im += I * ci + Q * cr;
    }
    return sqrt(re * re + im * im) / n;
}

static void LIBUSB_CALL cb(struct libusb_transfer *x) {
    if (x->status == LIBUSB_TRANSFER_COMPLETED && x->actual_length >= 8192) {
        double lv = tone_level(x->buffer, x->actual_length);
        if (t_seen == 0 && ((want_on && lv > thresh_on) || (!want_on && lv < thresh_off))) t_seen = now();
    }
    if (running) libusb_submit_transfer(x);
}

int main(int argc, char **argv) {
    long rate = argc > 1 ? atol(argv[1]) : 22000000;
    int trials = argc > 2 ? atoi(argv[2]) : 10;
    int nx = argc > 3 ? atoi(argv[3]) : 32;
    int xlen = (argc > 4 ? atoi(argv[4]) : 256) * 1024;
    long tone = rate / 8;

    glob_t g;
    if (glob("/dev/cu.wchusbserial*", 0, NULL, &g) || g.gl_pathc == 0) { fprintf(stderr, "no console\n"); return 1; }
    ser = open(g.gl_pathv[0], O_RDWR | O_NOCTTY);
    struct termios tio;
    tcgetattr(ser, &tio); cfmakeraw(&tio); cfsetspeed(&tio, B115200); tcsetattr(ser, TCSANOW, &tio);

    libusb_context *ctx;
    libusb_init(&ctx);
    libusb_device **list;
    ssize_t n = libusb_get_device_list(ctx, &list);
    libusb_device_handle *h = NULL;
    int ifnum = -1; unsigned char ep = 0;
    for (ssize_t i = 0; i < n && !h; i++) {
        struct libusb_device_descriptor dd;
        libusb_get_device_descriptor(list[i], &dd);
        if (dd.idVendor != 0x0456) continue;
        struct libusb_config_descriptor *cfg;
        if (libusb_get_active_config_descriptor(list[i], &cfg)) continue;
        for (int k = 0; k < cfg->bNumInterfaces; k++) {
            const struct libusb_interface_descriptor *id = &cfg->interface[k].altsetting[0];
            if (id->bInterfaceClass == 0xff && id->bInterfaceSubClass == 0x5a) { ifnum = id->bInterfaceNumber; ep = id->endpoint[0].bEndpointAddress; }
        }
        libusb_free_config_descriptor(cfg);
        if (ifnum >= 0 && libusb_open(list[i], &h)) h = NULL;
    }
    if (!h || libusb_claim_interface(h, ifnum)) { fprintf(stderr, "zc interface not available\n"); return 1; }
    for (int i = 0; i < nx; i++) {
        struct libusb_transfer *x = libusb_alloc_transfer(0);
        libusb_fill_bulk_transfer(x, h, ep, malloc(xlen), xlen, cb, NULL, 2000);
        libusb_submit_transfer(x);
    }
    char on[128], off[] = "echo 0 0 0 0 > /sys/kernel/debug/iio/iio:device0/bist_tone";
    snprintf(on, sizeof on, "echo 2 %ld 0 0 > /sys/kernel/debug/iio/iio:device0/bist_tone", tone); // 2 = BIST_INJ_RX
    console(off);
    double settle = now() + 1.0;
    while (now() < settle) { struct timeval tv = {0, 50000}; libusb_handle_events_timeout(ctx, &tv); }

    double sum = 0, mn = 1e9, mx = 0;
    int ok = 0;
    for (int t = 0; t < trials; t++) {
        for (int phase = 1; phase >= 0; phase--) {
            want_on = phase; t_seen = 0;
            double t0 = now();
            console(phase ? on : off);
            double tq = now();
            while (t_seen == 0 && now() - t0 < 3.0) { struct timeval tv = {0, 2000}; libusb_handle_events_timeout(ctx, &tv); }
            if (phase && t_seen > 0) {
                double lat = (t_seen - tq) * 1000;
                printf("trial %d: tone seen %.1f ms after command (uart write %.1f ms)\n", t, lat, (tq - t0) * 1000);
                sum += lat; ok++; if (lat < mn) mn = lat; if (lat > mx) mx = lat;
            } else if (phase) printf("trial %d: tone NOT seen\n", t);
            double hold = now() + 0.3;
            while (now() < hold) { struct timeval tv = {0, 20000}; libusb_handle_events_timeout(ctx, &tv); }
        }
    }
    console(off);
    if (ok) printf("LATENCY ms: mean %.1f min %.1f max %.1f over %d trials (xfer %d x %d KiB)\n", sum / ok, mn, mx, ok, nx, xlen / 1024);
    running = 0;
    return 0;
}
