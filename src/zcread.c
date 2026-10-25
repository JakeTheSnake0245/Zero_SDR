// zcread: host-side reader for zcstream. Many async bulk-IN transfers in flight (libusb).
// usage: zcread SECONDS [NXFER] [XFER_KIB] [OUTFILE]
#include <libusb-1.0/libusb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static unsigned long long total;
static int running = 1, nfail;
static FILE *out;

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }

static void LIBUSB_CALL cb(struct libusb_transfer *x) {
    if (x->status == LIBUSB_TRANSFER_COMPLETED) {
        total += x->actual_length;
        if (out) fwrite(x->buffer, 1, x->actual_length, out);
    } else if (x->status != LIBUSB_TRANSFER_TIMED_OUT) {
        nfail++;
    }
    if (running && libusb_submit_transfer(x) != 0) nfail++;
}

int main(int argc, char **argv) {
    double secs = argc > 1 ? atof(argv[1]) : 10;
    int nx = argc > 2 ? atoi(argv[2]) : 32;
    int xlen = (argc > 3 ? atoi(argv[3]) : 256) * 1024;
    if (argc > 4) out = fopen(argv[4], "wb");
    libusb_context *ctx;
    libusb_init(&ctx);
    libusb_device **list;
    ssize_t n = libusb_get_device_list(ctx, &list);
    libusb_device_handle *h = NULL;
    int ifnum = -1;
    unsigned char ep = 0;
    for (ssize_t i = 0; i < n && !h; i++) {
        struct libusb_device_descriptor dd;
        libusb_get_device_descriptor(list[i], &dd);
        if (dd.idVendor != 0x0456) continue;
        struct libusb_config_descriptor *cfg;
        if (libusb_get_active_config_descriptor(list[i], &cfg)) continue;
        for (int k = 0; k < cfg->bNumInterfaces; k++) {
            const struct libusb_interface_descriptor *id = &cfg->interface[k].altsetting[0];
            if (id->bInterfaceClass == 0xff && id->bInterfaceSubClass == 0x5a && id->bNumEndpoints == 1) {
                ifnum = id->bInterfaceNumber;
                ep = id->endpoint[0].bEndpointAddress;
            }
        }
        libusb_free_config_descriptor(cfg);
        if (ifnum >= 0 && libusb_open(list[i], &h)) h = NULL;
    }
    libusb_free_device_list(list, 1);
    if (!h) { fprintf(stderr, "zero-copy interface not found\n"); return 1; }
    int r = libusb_claim_interface(h, ifnum);
    if (r) { fprintf(stderr, "claim interface %d: %s\n", ifnum, libusb_error_name(r)); return 1; }
    fprintf(stderr, "interface %d ep 0x%02x: %d transfers x %d KiB\n", ifnum, ep, nx, xlen / 1024);

    struct libusb_transfer **xs = calloc(nx, sizeof *xs);
    for (int i = 0; i < nx; i++) {
        xs[i] = libusb_alloc_transfer(0);
        libusb_fill_bulk_transfer(xs[i], h, ep, malloc(xlen), xlen, cb, NULL, 2000);
        libusb_submit_transfer(xs[i]);
    }
    double t0 = now(), tl = t0;
    unsigned long long last = 0;
    while (now() - t0 < secs) {
        struct timeval tv = { 0, 100000 };
        libusb_handle_events_timeout(ctx, &tv);
        double t = now();
        if (t - tl >= 1.0) {
            printf("t=%.0fs %.2f MB/s (inst %.2f) total %.1f MB fails %d\n", t - t0, total / (t - t0) / 1e6, (total - last) / (t - tl) / 1e6, total / 1e6, nfail);
            fflush(stdout);
            last = total; tl = t;
        }
    }
    running = 0;
    double dt = now() - t0;
    printf("RESULT bytes=%llu secs=%.2f MBps=%.3f fails=%d\n", total, dt, total / dt / 1e6, nfail);
    for (int i = 0; i < nx; i++) libusb_cancel_transfer(xs[i]);
    struct timeval tv = { 0, 300000 };
    libusb_handle_events_timeout(ctx, &tv);
    libusb_release_interface(h, ifnum);
    libusb_close(h);
    if (out) fclose(out);
    return 0;
}
