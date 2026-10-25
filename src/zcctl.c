// zcctl: command-line access to the zcd control plane.
// usage: zcctl status | get ID | set ID VALUE | stream RXTXBITS | arm 0|1 | dump
#include <libusb-1.0/libusb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "zcproto.h"

static libusb_device_handle *h;
static int ifn = -1;

static int open_zc(libusb_context *ctx) {
    libusb_device **list;
    ssize_t n = libusb_get_device_list(ctx, &list);
    for (ssize_t i = 0; i < n && !h; i++) {
        struct libusb_device_descriptor dd;
        if (libusb_get_device_descriptor(list[i], &dd) || dd.idVendor != ZC_VID) continue;
        struct libusb_config_descriptor *cfg;
        if (libusb_get_active_config_descriptor(list[i], &cfg)) continue;
        for (int k = 0; k < cfg->bNumInterfaces; k++) {
            const struct libusb_interface_descriptor *id = &cfg->interface[k].altsetting[0];
            if (id->bInterfaceClass == ZC_IF_CLASS && id->bInterfaceSubClass == ZC_IF_SUBCLASS && id->bInterfaceProtocol == ZC_IF_PROTOCOL) ifn = id->bInterfaceNumber;
        }
        libusb_free_config_descriptor(cfg);
        if (ifn >= 0 && libusb_open(list[i], &h)) h = NULL;
    }
    libusb_free_device_list(list, 1);
    return h && !libusb_claim_interface(h, ifn) ? 0 : -1;
}
static int get(int id, long long *v) {
    unsigned long long le = 0;
    int r = libusb_control_transfer(h, 0xC1, ZC_REQ_GET, id, ifn, (unsigned char *)&le, 8, 1000);
    *v = (long long)le;
    return r == 8 ? 0 : r;
}
static void status(void) {
    struct zc_status s;
    int r = libusb_control_transfer(h, 0xC1, ZC_REQ_STATUS, 0, ifn, (unsigned char *)&s, sizeof s, 1000);
    if (r != (int)sizeof s) { printf("status failed: %s\n", libusb_error_name(r)); return; }
    printf("rx_bytes %llu tx_bytes %llu rx_ovf %u tx_unf %u flags 0x%x (rx %d tx %d armed %d wd %d) rx_bits %u tx_bits %u rate %llu last_err %u (%s)\n",
           (unsigned long long)s.rx_bytes, (unsigned long long)s.tx_bytes, s.rx_ovf, s.tx_unf, s.flags, !!(s.flags & ZC_F_RX_RUN), !!(s.flags & ZC_F_TX_RUN),
           !!(s.flags & ZC_F_TX_ARMED), !!(s.flags & ZC_F_TX_WD), s.rx_bits, s.tx_bits, (unsigned long long)s.rate_hz, s.last_err, s.last_err ? strerror(s.last_err) : "-");
}

int main(int argc, char **argv) {
    libusb_context *ctx;
    libusb_init(&ctx);
    if (open_zc(ctx)) { fprintf(stderr, "zero-copy interface not available\n"); return 1; }
    const char *c = argc > 1 ? argv[1] : "status";
    int rc = 0;
    if (!strcmp(c, "status")) status();
    else if (!strcmp(c, "get") && argc > 2) { long long v; int r = get(atoi(argv[2]), &v); if (r) { printf("get %s failed: %s\n", argv[2], libusb_error_name(r)); rc = 1; } else printf("%lld\n", v); }
    else if (!strcmp(c, "dump")) {
        for (int id = 1; id < ZC_P_MAX; id++) { long long v; int r = get(id, &v); if (r) printf("%2d: FAILED (%s)\n", id, libusb_error_name(r)); else printf("%2d: %lld\n", id, v); }
        status();
    } else if (!strcmp(c, "set") && argc > 3) {
        unsigned long long le = (unsigned long long)atoll(argv[3]);
        int r = libusb_control_transfer(h, 0x41, ZC_REQ_SET, atoi(argv[2]), ifn, (unsigned char *)&le, 8, 1000);
        printf("set %s=%s: %s\n", argv[2], argv[3], r == 8 ? "sent" : libusb_error_name(r));
        status();
    } else if ((!strcmp(c, "stream") || !strcmp(c, "arm")) && argc > 2) {
        int r = libusb_control_transfer(h, 0x41, !strcmp(c, "arm") ? ZC_REQ_ARM : ZC_REQ_STREAM, atoi(argv[2]), ifn, NULL, 0, 1000);
        printf("%s %s: %s\n", c, argv[2], r == 0 ? "ok" : libusb_error_name(r));
        status();
    } else { fprintf(stderr, "usage: zcctl status | get ID | set ID V | stream BITS | arm 0|1 | dump\n"); rc = 2; }
    libusb_release_interface(h, ifn);
    libusb_close(h);
    libusb_exit(ctx);
    return rc;
}
