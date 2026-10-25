// zcproto.h — wire protocol between the board daemon (zcstream) and host drivers (SoapyNanoZC).
// One USB interface (class 0xFF, subclass 0x5A, protocol 0x02):
//   bulk IN  0x81 : RX samples (CS8 = I8,Q8 per sample; CS16 = I16,Q16 little-endian, 12-bit in 16)
//   bulk OUT 0x02 : TX samples (same formats)
//   ep0 vendor requests to the interface (wIndex = interface number):
//     ZC_REQ_SET    0x41/0x01  wValue=param  data: int64 LE          set a parameter
//     ZC_REQ_GET    0xC1/0x02  wValue=param  data: int64 LE          read a parameter
//     ZC_REQ_STREAM 0x41/0x10  wValue=bit0 RX run, bit1 TX run       start/stop streams (no data)
//     ZC_REQ_ARM    0x41/0x12  wValue=1 arm, 0 disarm                TX RF enable (checked on the board)
//     ZC_REQ_STATUS 0xC1/0x20  data: struct zc_status                counters
#ifndef ZCPROTO_H
#define ZCPROTO_H
#include <stdint.h>

#define ZC_VID 0x0456
#define ZC_PID 0xb673
#define ZC_IF_CLASS 0xff
#define ZC_IF_SUBCLASS 0x5a
#define ZC_IF_PROTOCOL 0x02

#define ZC_REQ_SET 0x01
#define ZC_REQ_GET 0x02
#define ZC_REQ_STREAM 0x10
#define ZC_REQ_ARM 0x12
#define ZC_REQ_STATUS 0x20

enum zc_param {
    ZC_P_RX_LO_HZ = 1,
    ZC_P_TX_LO_HZ = 2,
    ZC_P_RATE_HZ = 3,          // shared RX/TX sample rate (AD936x FDD)
    ZC_P_RX_BW_HZ = 4,
    ZC_P_TX_BW_HZ = 5,
    ZC_P_RX_GAIN_MODE = 6,     // 0 manual, 1 slow_attack, 2 fast_attack, 3 hybrid
    ZC_P_RX_GAIN_MDB = 7,      // milli-dB
    ZC_P_TX_ATTEN_MDB = 8,     // milli-dB, <= 0 (e.g. -10000 = -10 dB)
    ZC_P_RX_BITS = 9,          // 8 or 16 (wire format)
    ZC_P_TX_BITS = 10,
    ZC_P_RX_NBUF = 11,
    ZC_P_RX_BUF_KIB = 12,
    ZC_P_TX_NBUF = 13,
    ZC_P_TX_BUF_KIB = 14,
    ZC_P_TX_WD_MS = 15,        // TX watchdog: disarm if no TX data for this long while armed
    ZC_P_TX_ALLOW_MIN_HZ = 16, // TX LO allow-list (default: driver's full TX LO range)
    ZC_P_TX_ALLOW_MAX_HZ = 17,
    ZC_P_TX_ATTEN_CEIL_MDB = 18, // least attenuation allowed while armed (default 0 = full power)
    ZC_P_RX_PORT = 19,         // 0 A_BALANCED, 1 B_BALANCED, 2 C_BALANCED
    ZC_P_TX_PORT = 20,         // 0 A, 1 B
    ZC_P_RX_RSSI_MDB = 21,     // get only
    ZC_P_TEMP_MC = 22,         // get only, AD936x temperature, milli-degC
    ZC_P_RX_QUAD_TRACK = 23,   // 0/1
    ZC_P_RX_RF_DC_TRACK = 24,  // 0/1
    ZC_P_RX_BB_DC_TRACK = 25,  // 0/1
    ZC_P_LO_MIN_HZ = 26,       // get only: driver LO range
    ZC_P_LO_MAX_HZ = 27,
    ZC_P_ZYNQ_TEMP_MC = 28,    // get only: Zynq XADC die temperature, milli-degC
    ZC_P_MAX
};

struct zc_status {          // little-endian, 64 bytes
    uint64_t rx_bytes;      // bytes delivered to USB since RX start (sample counter = rx_bytes / bytes_per_sample)
    uint64_t tx_bytes;      // bytes consumed by the DAC since TX start
    uint32_t rx_ovf;        // ADC DMA overflow events (RX samples lost)
    uint32_t tx_unf;        // DAC DMA underflow events (TX starved)
    uint32_t flags;         // bit0 rx running, bit1 tx running, bit2 tx armed, bit3 watchdog tripped
    uint32_t rx_bits;
    uint32_t tx_bits;
    int32_t temp_mc;
    uint64_t rate_hz;
    uint32_t last_err;      // errno of the last failed request
    uint32_t reserved[3];
} __attribute__((packed));

#define ZC_F_RX_RUN 1u
#define ZC_F_TX_RUN 2u
#define ZC_F_TX_ARMED 4u
#define ZC_F_TX_WD 8u
#endif
