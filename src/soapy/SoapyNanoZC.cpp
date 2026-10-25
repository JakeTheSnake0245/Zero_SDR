// SoapyNanoZC: SoapySDR driver for the PlutoSDR Nano zero-copy interface (board daemon: zcd, protocol: zcproto.h).
// RX/TX samples ride one vendor USB interface (bulk IN/OUT); all controls are vendor requests on ep0.
//
// Stream args: bits=8|16 (wire format; default 8 above 10.5 MS/s, else 16), buffers=N, buffer_kib=K (board DMABUFs),
//              xfers=N, xfer_kib=K (host libusb transfers).
// TX stays disarmed until writeSetting("tx_arm", "true"); the board enforces its allow-list/ceiling/watchdog.
#include <SoapySDR/Device.hpp>
#include <SoapySDR/Formats.hpp>
#include <SoapySDR/Logger.hpp>
#include <SoapySDR/Registry.hpp>
#include <libusb-1.0/libusb.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

extern "C" {
#include "../zcproto.h"
}

namespace {

struct Xfer { libusb_transfer *t = nullptr; std::vector<uint8_t> buf; };

struct ZCStream {
    int dir;
    std::string format; // CS8 / CS16 / CF32
    int bits;           // wire bits
    size_t bps;         // wire bytes per sample
    std::vector<Xfer> xfers;
    std::mutex m;
    std::condition_variable cv;
    std::deque<Xfer *> ready;   // RX: completed with data; TX: free buffers
    Xfer *cur = nullptr;        // RX: buffer being read; TX: buffer being filled
    size_t curOff = 0, curLen = 0;
    long long samples = 0;      // samples handed to/received from the user (timestamps)
    double rate = 1;            // cached sample rate: never make a control transfer while holding m (libusb event-thread deadlock)
    std::atomic<bool> active{false};
    std::atomic<int> inflight{0};
    uint32_t lastOvf = 0, lastUnf = 0;
    std::chrono::steady_clock::time_point lastStatus;
    // RX: free-running USB reader fills this host ring; readStream serves from it. Decouples USB draining
    // from the consumer (SDR++), so a brief DSP hiccup never starves the board into an overflow.
    std::vector<uint8_t> ring; size_t rcap = 0, rhead = 0, rtail = 0, rcount = 0; // bytes
    std::atomic<bool> rxoverrun{false};
};

class NanoZC : public SoapySDR::Device {
public:
    explicit NanoZC(const SoapySDR::Kwargs &args) {
        if (libusb_init(&ctx_)) throw std::runtime_error("libusb_init failed");
        libusb_device **list;
        ssize_t n = libusb_get_device_list(ctx_, &list);
        const std::string wantSerial = args.count("serial") ? args.at("serial") : "";
        for (ssize_t i = 0; i < n && !h_; i++) {
            int ifn = -1; uint8_t in = 0, out = 0;
            if (!probe(list[i], ifn, in, out)) continue;
            libusb_device_handle *h;
            if (libusb_open(list[i], &h)) continue;
            std::string ser = serialOf(h, list[i]);
            if (!wantSerial.empty() && ser != wantSerial) { libusb_close(h); continue; }
            if (libusb_claim_interface(h, ifn)) { libusb_close(h); continue; }
            h_ = h; if_ = ifn; epIn_ = in; epOut_ = out; serial_ = ser;
        }
        libusb_free_device_list(list, 1);
        if (!h_) throw std::runtime_error("NanoZC: zero-copy interface not found (is zcd running?)");
        evRun_ = true;
        ev_ = std::thread([this] { while (evRun_) { timeval tv{0, 50000}; libusb_handle_events_timeout_completed(ctx_, &tv, nullptr); } });
        loMin_ = get(ZC_P_LO_MIN_HZ); loMax_ = get(ZC_P_LO_MAX_HZ);
        // The AD9361 driver rejects the exact bottom-edge LO (e.g. 46875000/46875001 Hz) even though
        // values a few Hz higher tune fine. A host that opens the device at the advertised range
        // minimum (e.g. SCEPTRE sets LO = range.min) then gets EINVAL and fails to connect. Nudge the
        // advertised/clamped minimum a few kHz above that dead zone; negligible loss (keeps 50 MHz 6 m).
        loMin_ += 4096;
        SoapySDR::logf(SOAPY_SDR_INFO, "NanoZC: interface %d, serial %s, LO %.3f-%.3f MHz", if_, serial_.c_str(), loMin_ / 1e6, loMax_ / 1e6);
    }

    ~NanoZC() override {
        try { ctrl(ZC_REQ_ARM, 0); ctrl(ZC_REQ_STREAM, 0); } catch (...) {}
        evRun_ = false;
        if (ev_.joinable()) ev_.join();
        if (h_) { libusb_release_interface(h_, if_); libusb_close(h_); }
        libusb_exit(ctx_);
    }

    static bool probe(libusb_device *d, int &ifn, uint8_t &in, uint8_t &out) {
        libusb_device_descriptor dd;
        if (libusb_get_device_descriptor(d, &dd) || dd.idVendor != ZC_VID) return false;
        libusb_config_descriptor *cfg;
        if (libusb_get_active_config_descriptor(d, &cfg)) return false;
        bool ok = false;
        for (int k = 0; k < cfg->bNumInterfaces && !ok; k++) {
            const libusb_interface_descriptor *id = &cfg->interface[k].altsetting[0];
            if (id->bInterfaceClass == ZC_IF_CLASS && id->bInterfaceSubClass == ZC_IF_SUBCLASS && id->bInterfaceProtocol == ZC_IF_PROTOCOL && id->bNumEndpoints == 2) {
                ifn = id->bInterfaceNumber; ok = true;
                for (int e = 0; e < 2; e++) {
                    uint8_t a = id->endpoint[e].bEndpointAddress;
                    if (a & 0x80) in = a; else out = a;
                }
            }
        }
        libusb_free_config_descriptor(cfg);
        return ok;
    }
    static std::string serialOf(libusb_device_handle *h, libusb_device *d) {
        libusb_device_descriptor dd; libusb_get_device_descriptor(d, &dd);
        unsigned char s[128] = {0};
        if (dd.iSerialNumber) libusb_get_string_descriptor_ascii(h, dd.iSerialNumber, s, sizeof s);
        return reinterpret_cast<char *>(s);
    }

    // ---- control plane ----
    void ctrl(uint8_t req, uint16_t val) {
        std::lock_guard<std::mutex> l(ctrlM_);
        int r = libusb_control_transfer(h_, 0x41, req, val, if_, nullptr, 0, 1000);
        if (r < 0) throw std::runtime_error(std::string("NanoZC request failed: ") + libusb_error_name(r) + lastErrText());
    }
    void set(int id, int64_t v) {
        std::lock_guard<std::mutex> l(ctrlM_);
        uint64_t le = static_cast<uint64_t>(v); // host is little-endian
        int r = libusb_control_transfer(h_, 0x41, ZC_REQ_SET, id, if_, reinterpret_cast<unsigned char *>(&le), 8, 1000);
        if (r != 8) throw std::runtime_error("NanoZC set failed");
    }
    int64_t get(int id) {
        std::lock_guard<std::mutex> l(ctrlM_);
        uint64_t le = 0;
        int r = libusb_control_transfer(h_, 0xC1, ZC_REQ_GET, id, if_, reinterpret_cast<unsigned char *>(&le), 8, 1000);
        if (r != 8) throw std::runtime_error("NanoZC get failed");
        return static_cast<int64_t>(le);
    }
    zc_status status() {
        std::lock_guard<std::mutex> l(ctrlM_);
        zc_status s{};
        int r = libusb_control_transfer(h_, 0xC1, ZC_REQ_STATUS, 0, if_, reinterpret_cast<unsigned char *>(&s), sizeof s, 1000);
        if (r != (int)sizeof s) throw std::runtime_error("NanoZC status failed");
        return s;
    }
    std::string lastErrText() {
        zc_status s{};
        int r = libusb_control_transfer(h_, 0xC1, ZC_REQ_STATUS, 0, if_, reinterpret_cast<unsigned char *>(&s), sizeof s, 1000);
        return r == (int)sizeof s && s.last_err ? std::string(" (board errno ") + std::strerror(s.last_err) + ")" : "";
    }
    // SET with board-side error check (the data stage acks before the board applies it)
    void setChecked(int id, int64_t v) {
        zc_status before = status();
        set(id, v);
        zc_status after = status();
        if (after.last_err && after.last_err != before.last_err) throw std::runtime_error("NanoZC: board rejected parameter " + std::to_string(id) + ": " + std::strerror(after.last_err));
    }

    // ---- identification ----
    std::string getDriverKey() const override { return "nanozc"; }
    std::string getHardwareKey() const override { return "PlutoSDR Nano (zero-copy)"; }
    SoapySDR::Kwargs getHardwareInfo() const override { return {{"serial", serial_}, {"interface", std::to_string(if_)}}; }
    size_t getNumChannels(const int) const override { return 1; }
    bool getFullDuplex(const int, const size_t) const override { return true; }

    // ---- antennas ----
    std::vector<std::string> listAntennas(const int dir, const size_t) const override {
        // The board wires only port A to the SMAs and its device tree locks the AD936x RF port selection to A.
        return dir == SOAPY_SDR_RX ? std::vector<std::string>{"A_BALANCED"} : std::vector<std::string>{"A"};
    }
    void setAntenna(const int dir, const size_t, const std::string &name) override {
        auto l = listAntennas(dir, 0);
        auto it = std::find(l.begin(), l.end(), name);
        if (it == l.end()) throw std::runtime_error("unknown antenna " + name);
        setChecked(dir == SOAPY_SDR_RX ? ZC_P_RX_PORT : ZC_P_TX_PORT, it - l.begin());
        ant_[dir] = name;
    }
    std::string getAntenna(const int dir, const size_t) const override { return ant_[dir].empty() ? listAntennas(dir, 0)[0] : ant_[dir]; }

    // ---- frequency ----
    void setFrequency(const int dir, const size_t, const double f, const SoapySDR::Kwargs &) override {
        // Clamp to the AD936x LO range. A host (e.g. SCEPTRE) often sets an out-of-range or 0 Hz
        // default frequency on open; forwarding that raw gets EINVAL from the board and the
        // connection fails. Clamping lets the device open; the host then tunes to a real frequency.
        double cf = f;
        if (cf < loMin_) cf = loMin_;
        else if (cf > loMax_) cf = loMax_;
        setChecked(dir == SOAPY_SDR_RX ? ZC_P_RX_LO_HZ : ZC_P_TX_LO_HZ, static_cast<int64_t>(cf));
    }
    double getFrequency(const int dir, const size_t) const override { return const_cast<NanoZC *>(this)->get(dir == SOAPY_SDR_RX ? ZC_P_RX_LO_HZ : ZC_P_TX_LO_HZ); }
    SoapySDR::RangeList getFrequencyRange(const int, const size_t) const override { return {SoapySDR::Range(loMin_, loMax_)}; }

    // ---- sample rate & bandwidth (AD936x: RX and TX share the rate) ----
    void setSampleRate(const int, const size_t, const double rate) override {
        // A rate write re-tunes the AD936x clocks even for the same value; skipping no-op writes keeps running streams intact.
        if (static_cast<int64_t>(get(ZC_P_RATE_HZ)) == static_cast<int64_t>(rate)) return;
        if ((rx_ && rx_->active) || (tx_ && tx_->active))
            SoapySDR::log(SOAPY_SDR_WARNING, "NanoZC: changing the sample rate while streaming re-tunes the AD936x; streams may restart");
        setChecked(ZC_P_RATE_HZ, static_cast<int64_t>(rate));
        double actual = get(ZC_P_RATE_HZ);
        for (ZCStream *z : {rx_, tx_}) if (z) { std::lock_guard<std::mutex> l(z->m); z->rate = actual; }
        if (rate > 22.0e6) SoapySDR::logf(SOAPY_SDR_WARNING, "NanoZC: %.2f MS/s exceeds the measured USB 2.0 budget (22 MS/s CS8, 10 MS/s CS16); expect overflows", rate / 1e6);
    }
    double getSampleRate(const int, const size_t) const override { return const_cast<NanoZC *>(this)->get(ZC_P_RATE_HZ); }
    SoapySDR::RangeList getSampleRateRange(const int, const size_t) const override { return {SoapySDR::Range(2083333, 61440000)}; }
    std::vector<double> listSampleRates(const int, const size_t) const override { return {2.5e6, 4e6, 5e6, 8e6, 10e6, 12e6, 16e6, 20e6, 21e6, 22e6}; }
    void setBandwidth(const int dir, const size_t, const double bw) override { setChecked(dir == SOAPY_SDR_RX ? ZC_P_RX_BW_HZ : ZC_P_TX_BW_HZ, static_cast<int64_t>(bw)); }
    double getBandwidth(const int dir, const size_t) const override { return const_cast<NanoZC *>(this)->get(dir == SOAPY_SDR_RX ? ZC_P_RX_BW_HZ : ZC_P_TX_BW_HZ); }
    SoapySDR::RangeList getBandwidthRange(const int, const size_t) const override { return {SoapySDR::Range(200e3, 56e6)}; }

    // ---- gain: RX hardware gain (dB); TX gain = 89.75 + attenuation ----
    std::vector<std::string> listGains(const int, const size_t) const override { return {"PGA"}; }
    bool hasGainMode(const int dir, const size_t) const override { return dir == SOAPY_SDR_RX; }
    void setGainMode(const int dir, const size_t, const bool automatic) override {
        if (dir != SOAPY_SDR_RX) return;
        setChecked(ZC_P_RX_GAIN_MODE, automatic ? agcMode_ : 0);
        agc_ = automatic;
    }
    bool getGainMode(const int dir, const size_t) const override { return dir == SOAPY_SDR_RX && agc_; }
    void setGain(const int dir, const size_t ch, const double g) override { setGain(dir, ch, "PGA", g); }
    void setGain(const int dir, const size_t, const std::string &, const double g) override {
        if (dir == SOAPY_SDR_RX) {
            // Setting a manual gain requires MANUAL gain mode. The board boots in AGC, and a host
            // (e.g. SCEPTRE) may set a gain on open before touching the mode; writing manual gain while
            // in AGC returns EOPNOTSUPP. Always force manual first (standard Soapy setGain semantics);
            // a host that wants AGC calls setGainMode(true) separately.
            setGainMode(SOAPY_SDR_RX, 0, false);
            setChecked(ZC_P_RX_GAIN_MDB, static_cast<int64_t>(g * 1000));
        } else {
            double atten = std::min(0.0, std::max(-89.75, g - 89.75));
            setChecked(ZC_P_TX_ATTEN_MDB, static_cast<int64_t>(atten * 1000));
        }
    }
    double getGain(const int dir, const size_t ch) const override { return getGain(dir, ch, "PGA"); }
    double getGain(const int dir, const size_t, const std::string &) const override {
        auto *self = const_cast<NanoZC *>(this);
        return dir == SOAPY_SDR_RX ? self->get(ZC_P_RX_GAIN_MDB) / 1000.0 : 89.75 + self->get(ZC_P_TX_ATTEN_MDB) / 1000.0;
    }
    SoapySDR::Range getGainRange(const int dir, const size_t ch) const override { return getGainRange(dir, ch, "PGA"); }
    SoapySDR::Range getGainRange(const int dir, const size_t, const std::string &) const override {
        return dir == SOAPY_SDR_RX ? SoapySDR::Range(0, 73, 1) : SoapySDR::Range(0, 89.75, 0.25);
    }

    // ---- sensors ----
    std::vector<std::string> listSensors() const override { return {"temperature", "zynq_temperature", "rssi", "rx_overflows", "tx_underflows", "tx_armed", "tx_watchdog_tripped"}; }
    std::string readSensor(const std::string &k) const override {
        auto *self = const_cast<NanoZC *>(this);
        if (k == "temperature") return std::to_string(self->get(ZC_P_TEMP_MC) / 1000.0);
        if (k == "zynq_temperature") return std::to_string(self->get(ZC_P_ZYNQ_TEMP_MC) / 1000.0);
        if (k == "rssi") return std::to_string(-self->get(ZC_P_RX_RSSI_MDB) / 1000.0);
        zc_status s = self->status();
        if (k == "rx_overflows") return std::to_string(s.rx_ovf);
        if (k == "tx_underflows") return std::to_string(s.tx_unf);
        if (k == "tx_armed") return (s.flags & ZC_F_TX_ARMED) ? "true" : "false";
        if (k == "tx_watchdog_tripped") return (s.flags & ZC_F_TX_WD) ? "true" : "false";
        throw std::runtime_error("unknown sensor " + k);
    }

    // ---- settings ----
    SoapySDR::ArgInfoList getSettingInfo() const override {
        SoapySDR::ArgInfoList l;
        auto add = [&](const char *k, const char *d, const char *v) { SoapySDR::ArgInfo a; a.key = k; a.description = d; a.value = v; l.push_back(a); };
        add("tx_arm", "Enable RF output (board checks allow-list, ceiling, watchdog)", "false");
        add("agc_mode", "RX AGC flavour when gain mode is automatic: slow | fast | hybrid", "slow");
        add("tx_watchdog_ms", "Disarm TX if no TX data for this long", "250");
        add("tx_allow_min_hz", "TX LO allow-list lower bound", "");
        add("tx_allow_max_hz", "TX LO allow-list upper bound", "");
        add("tx_atten_ceiling_db", "Least TX attenuation allowed while armed (0 = full power)", "0");
        add("quadrature_tracking", "RX quadrature tracking", "true");
        add("rf_dc_tracking", "RX RF DC offset tracking", "true");
        add("bb_dc_tracking", "RX BB DC offset tracking", "true");
        return l;
    }
    void writeSetting(const std::string &k, const std::string &v) override {
        auto tf = [&] { return v == "true" || v == "1"; };
        if (k == "tx_arm") { ctrl(ZC_REQ_ARM, tf() ? 1 : 0); return; }
        if (k == "agc_mode") { agcMode_ = v == "fast" ? 2 : v == "hybrid" ? 3 : 1; if (agc_) setChecked(ZC_P_RX_GAIN_MODE, agcMode_); return; }
        if (k == "tx_watchdog_ms") { setChecked(ZC_P_TX_WD_MS, std::stoll(v)); return; }
        if (k == "tx_allow_min_hz") { setChecked(ZC_P_TX_ALLOW_MIN_HZ, static_cast<int64_t>(std::stod(v))); return; }
        if (k == "tx_allow_max_hz") { setChecked(ZC_P_TX_ALLOW_MAX_HZ, static_cast<int64_t>(std::stod(v))); return; }
        if (k == "tx_atten_ceiling_db") { setChecked(ZC_P_TX_ATTEN_CEIL_MDB, static_cast<int64_t>(std::stod(v) * 1000)); return; }
        if (k == "quadrature_tracking") { setChecked(ZC_P_RX_QUAD_TRACK, tf()); return; }
        if (k == "rf_dc_tracking") { setChecked(ZC_P_RX_RF_DC_TRACK, tf()); return; }
        if (k == "bb_dc_tracking") { setChecked(ZC_P_RX_BB_DC_TRACK, tf()); return; }
        throw std::runtime_error("unknown setting " + k);
    }
    std::string readSetting(const std::string &k) const override {
        auto *self = const_cast<NanoZC *>(this);
        if (k == "tx_arm") return (self->status().flags & ZC_F_TX_ARMED) ? "true" : "false";
        if (k == "tx_watchdog_ms") return std::to_string(self->get(ZC_P_TX_WD_MS));
        if (k == "tx_allow_min_hz") return std::to_string(self->get(ZC_P_TX_ALLOW_MIN_HZ));
        if (k == "tx_allow_max_hz") return std::to_string(self->get(ZC_P_TX_ALLOW_MAX_HZ));
        if (k == "tx_atten_ceiling_db") return std::to_string(self->get(ZC_P_TX_ATTEN_CEIL_MDB) / 1000.0);
        return "";
    }

    // ---- streams ----
    std::vector<std::string> getStreamFormats(const int, const size_t) const override { return {SOAPY_SDR_CS8, SOAPY_SDR_CS16, SOAPY_SDR_CF32}; }
    std::string getNativeStreamFormat(const int dir, const size_t, double &fullScale) const override {
        double rate = const_cast<NanoZC *>(this)->get(ZC_P_RATE_HZ);
        if (rate > 10.5e6) { fullScale = 128; return SOAPY_SDR_CS8; }
        fullScale = dir == SOAPY_SDR_RX ? 2048 : 32768;
        return SOAPY_SDR_CS16;
    }
    SoapySDR::ArgInfoList getStreamArgsInfo(const int, const size_t) const override {
        SoapySDR::ArgInfoList l;
        auto add = [&](const char *k, const char *d, const char *v) { SoapySDR::ArgInfo a; a.key = k; a.description = d; a.value = v; l.push_back(a); };
        add("bits", "Wire format: 8 (up to ~22 MS/s) or 16 (up to ~10 MS/s)", "auto");
        add("buffers", "Board DMABUF count", "32");
        add("buffer_kib", "Board DMABUF size (KiB); smaller = lower latency", "64");
        add("xfers", "Host USB transfers in flight", "32");
        add("xfer_kib", "Host USB transfer size (KiB)", "64");
        add("ring_mb", "Host RX smoothing-buffer size (MiB); default ~0.5 s of signal", "auto");
        return l;
    }

    SoapySDR::Stream *setupStream(const int dir, const std::string &format, const std::vector<size_t> &channels, const SoapySDR::Kwargs &args) override {
        if (channels.size() > 1 || (channels.size() == 1 && channels[0] != 0)) throw std::runtime_error("NanoZC: one channel per direction");
        if (format != SOAPY_SDR_CS8 && format != SOAPY_SDR_CS16 && format != SOAPY_SDR_CF32) throw std::runtime_error("NanoZC: format " + format + " not supported");
        auto *s = new ZCStream;
        s->dir = dir; s->format = format;
        double rate = get(ZC_P_RATE_HZ);
        std::string b = args.count("bits") ? args.at("bits") : "auto";
        s->bits = b == "8" ? 8 : b == "16" ? 16 : (format == SOAPY_SDR_CS8 ? 8 : format == SOAPY_SDR_CS16 ? 16 : (rate > 10.5e6 ? 8 : 16));
        s->bps = s->bits == 8 ? 2 : 4;
        auto num = [&](const char *k, long d) { return args.count(k) ? std::stol(args.at(k)) : d; };
        long nbuf = num("buffers", 32), bkib = num("buffer_kib", 64), nx = num("xfers", 32), xkib = num("xfer_kib", 64);
        if (dir == SOAPY_SDR_TX) xkib = bkib; // TX transfers map 1:1 onto board DMABUFs (full buffers only)
        bool rx = dir == SOAPY_SDR_RX;
        if (rx) {   // host smoothing ring: default ~0.5 s of signal, so an SDR++ DSP hiccup can't starve the board
            double ringMb = args.count("ring_mb") ? std::stod(args.at("ring_mb")) : std::max(16.0, get(ZC_P_RATE_HZ) * s->bps * 0.5 / 1e6);
            s->rcap = ((size_t)(ringMb * 1024 * 1024) / (xkib * 1024) + 1) * xkib * 1024; // whole transfers
            s->ring.assign(s->rcap, 0);
        }
        setChecked(rx ? ZC_P_RX_BITS : ZC_P_TX_BITS, s->bits);
        setChecked(rx ? ZC_P_RX_NBUF : ZC_P_TX_NBUF, nbuf);
        setChecked(rx ? ZC_P_RX_BUF_KIB : ZC_P_TX_BUF_KIB, bkib);
        s->xfers.resize(nx);
        for (auto &x : s->xfers) {
            x.buf.resize(static_cast<size_t>(xkib) * 1024);
            x.t = libusb_alloc_transfer(0);
            libusb_fill_bulk_transfer(x.t, h_, rx ? epIn_ : epOut_, x.buf.data(), static_cast<int>(x.buf.size()), rx ? &NanoZC::rxDone : &NanoZC::txDone, s, 0);
            x.t->user_data = s;
            if (!rx) s->ready.push_back(&x);
        }
        SoapySDR::logf(SOAPY_SDR_INFO, "NanoZC %s stream: %s over %d-bit wire, board %ld x %ld KiB, host %ld x %ld KiB",
                       rx ? "RX" : "TX", format.c_str(), s->bits, nbuf, bkib, nx, xkib);
        (rx ? rx_ : tx_) = s;
        return reinterpret_cast<SoapySDR::Stream *>(s);
    }
    void closeStream(SoapySDR::Stream *st) override {
        auto *s = reinterpret_cast<ZCStream *>(st);
        deactivateStream(st, 0, 0);
        for (auto &x : s->xfers) libusb_free_transfer(x.t);
        if (s == rx_) rx_ = nullptr;
        if (s == tx_) tx_ = nullptr;
        delete s;
    }
    size_t getStreamMTU(SoapySDR::Stream *st) const override { auto *s = reinterpret_cast<ZCStream *>(st); return s->xfers[0].buf.size() / s->bps; }

    int activateStream(SoapySDR::Stream *st, const int, const long long, const size_t) override {
        auto *s = reinterpret_cast<ZCStream *>(st);
        if (s->active) return 0;
        s->rate = get(ZC_P_RATE_HZ);
        s->active = true; s->samples = 0; s->cur = nullptr;
        { std::lock_guard<std::mutex> l(s->m); s->rhead = s->rtail = s->rcount = 0; s->rxoverrun = false; }
        updateStreams();
        if (s->dir == SOAPY_SDR_RX)
            for (auto &x : s->xfers) { s->inflight++; if (libusb_submit_transfer(x.t)) s->inflight--; }
        return 0;
    }
    int deactivateStream(SoapySDR::Stream *st, const int, const long long) override {
        auto *s = reinterpret_cast<ZCStream *>(st);
        if (!s->active) return 0;
        if (s->dir == SOAPY_SDR_TX) flushTx(s, 500000);
        s->active = false;
        updateStreams();
        for (auto &x : s->xfers) libusb_cancel_transfer(x.t);
        for (int i = 0; i < 100 && s->inflight > 0; i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        std::lock_guard<std::mutex> l(s->m);
        s->ready.clear(); s->cur = nullptr; s->rhead = s->rtail = s->rcount = 0;
        if (s->dir == SOAPY_SDR_TX) for (auto &x : s->xfers) s->ready.push_back(&x);
        return 0;
    }

    int readStream(SoapySDR::Stream *st, void *const *buffs, const size_t numElems, int &flags, long long &timeNs, const long timeoutUs) override {
        auto *s = reinterpret_cast<ZCStream *>(st);
        flags = 0;
        if (s->rxoverrun.exchange(false)) return SOAPY_SDR_OVERFLOW;
        std::unique_lock<std::mutex> l(s->m);
        if (s->rcount < s->bps && !s->cv.wait_for(l, std::chrono::microseconds(timeoutUs), [s] { return s->rcount >= s->bps; }))
            return SOAPY_SDR_TIMEOUT;
        size_t contig = std::min(s->rcount, s->rcap - s->rtail);  // bytes contiguous to the ring end
        size_t n = std::min(contig / s->bps, numElems);
        convertIn(s, s->ring.data() + s->rtail, buffs[0], n);
        s->rtail = (s->rtail + n * s->bps) % s->rcap; s->rcount -= n * s->bps;
        timeNs = static_cast<long long>(s->samples * 1e9 / s->rate);
        flags |= SOAPY_SDR_HAS_TIME;
        s->samples += n;
        return static_cast<int>(n);
    }

    int writeStream(SoapySDR::Stream *st, const void *const *buffs, const size_t numElems, int &flags, const long long, const long timeoutUs) override {
        auto *s = reinterpret_cast<ZCStream *>(st);
        std::unique_lock<std::mutex> l(s->m);
        if (!s->cur) {
            if (!s->cv.wait_for(l, std::chrono::microseconds(timeoutUs), [s] { return !s->ready.empty(); })) return SOAPY_SDR_TIMEOUT;
            s->cur = s->ready.front(); s->ready.pop_front(); s->curOff = 0; s->curLen = s->cur->buf.size();
        }
        size_t room = (s->curLen - s->curOff) / s->bps, n = std::min(room, numElems);
        convertOut(s, buffs[0], s->cur->buf.data() + s->curOff, n);
        s->curOff += n * s->bps; s->samples += n;
        bool burstEnd = (flags & SOAPY_SDR_END_BURST) && n == numElems;
        if (s->curOff >= s->curLen || burstEnd) {
            if (s->curOff < s->curLen) std::memset(s->cur->buf.data() + s->curOff, 0, s->curLen - s->curOff); // board needs full DMABUFs
            Xfer *x = s->cur; s->cur = nullptr;
            l.unlock();
            s->inflight++;
            if (libusb_submit_transfer(x->t)) { s->inflight--; std::lock_guard<std::mutex> g(s->m); s->ready.push_back(x); return SOAPY_SDR_STREAM_ERROR; }
        }
        return static_cast<int>(n);
    }

    int readStreamStatus(SoapySDR::Stream *st, size_t &, int &flags, long long &, const long timeoutUs) override {
        auto *s = reinterpret_cast<ZCStream *>(st);
        flags = 0;
        auto end = std::chrono::steady_clock::now() + std::chrono::microseconds(timeoutUs);
        do {
            if (pollCounters(s, true)) return SOAPY_SDR_UNDERFLOW;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        } while (std::chrono::steady_clock::now() < end);
        return SOAPY_SDR_TIMEOUT;
    }

    bool hasHardwareTime(const std::string &what) const override { return what.empty(); }
    long long getHardwareTime(const std::string &) const override {
        zc_status s = const_cast<NanoZC *>(this)->status();
        return s.rx_bits ? static_cast<long long>(s.rx_bytes / (s.rx_bits == 8 ? 2 : 4) * 1e9 / rate()) : 0;
    }

private:
    double rate() const { return const_cast<NanoZC *>(this)->get(ZC_P_RATE_HZ); }

    void updateStreams() { ctrl(ZC_REQ_STREAM, (rx_ && rx_->active ? 1 : 0) | (tx_ && tx_->active ? 2 : 0)); }

    // RX overflow / TX underflow from the board counters, checked at most every 100 ms
    bool pollCounters(ZCStream *s, bool tx = false) {
        auto t = std::chrono::steady_clock::now();
        if (t - s->lastStatus < std::chrono::milliseconds(100)) return false;
        s->lastStatus = t;
        zc_status z = status();
        // counters reset to 0 when the board (re)starts a stream: only an increase is a real event
        bool ev = !tx ? z.rx_ovf > s->lastOvf : z.tx_unf > s->lastUnf;
        s->lastOvf = z.rx_ovf; s->lastUnf = z.tx_unf;
        return ev;
    }

    void flushTx(ZCStream *s, long timeoutUs) {
        int f = SOAPY_SDR_END_BURST;
        if (s->cur && s->curOff) { const void *nothing[1] = {nullptr}; (void)nothing; std::unique_lock<std::mutex> l(s->m); std::memset(s->cur->buf.data() + s->curOff, 0, s->curLen - s->curOff); Xfer *x = s->cur; s->cur = nullptr; l.unlock(); s->inflight++; if (libusb_submit_transfer(x->t)) s->inflight--; }
        (void)f;
        auto end = std::chrono::steady_clock::now() + std::chrono::microseconds(timeoutUs);
        while (s->inflight > 0 && std::chrono::steady_clock::now() < end) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    static void convertIn(ZCStream *s, const uint8_t *src, void *dst, size_t n) {
        if (s->bits == 8) {
            const int8_t *p = reinterpret_cast<const int8_t *>(src);
            if (s->format == SOAPY_SDR_CS8) std::memcpy(dst, p, n * 2);
            else if (s->format == SOAPY_SDR_CS16) { auto *o = static_cast<int16_t *>(dst); for (size_t i = 0; i < 2 * n; i++) o[i] = static_cast<int16_t>(p[i] * 16); }
            else { auto *o = static_cast<float *>(dst); for (size_t i = 0; i < 2 * n; i++) o[i] = p[i] / 128.0f; }
        } else {
            const int16_t *p = reinterpret_cast<const int16_t *>(src); // 12-bit, LSB-aligned, sign-extended
            if (s->format == SOAPY_SDR_CS16) std::memcpy(dst, p, n * 4);
            else if (s->format == SOAPY_SDR_CS8) { auto *o = static_cast<int8_t *>(dst); for (size_t i = 0; i < 2 * n; i++) o[i] = static_cast<int8_t>(p[i] >> 4); }
            else { auto *o = static_cast<float *>(dst); for (size_t i = 0; i < 2 * n; i++) o[i] = p[i] / 2048.0f; }
        }
    }
    static int16_t clip16(float v) { v = v > 1.f ? 1.f : v < -1.f ? -1.f : v; return static_cast<int16_t>(v * 32767.f); }
    static int8_t clip8(float v) { v = v > 1.f ? 1.f : v < -1.f ? -1.f : v; return static_cast<int8_t>(v * 127.f); }
    static void convertOut(ZCStream *s, const void *src, uint8_t *dst, size_t n) {
        if (s->bits == 8) {
            auto *o = reinterpret_cast<int8_t *>(dst);
            if (s->format == SOAPY_SDR_CS8) std::memcpy(o, src, n * 2);
            else if (s->format == SOAPY_SDR_CS16) { auto *p = static_cast<const int16_t *>(src); for (size_t i = 0; i < 2 * n; i++) o[i] = static_cast<int8_t>(p[i] >> 8); }
            else { auto *p = static_cast<const float *>(src); for (size_t i = 0; i < 2 * n; i++) o[i] = clip8(p[i]); }
        } else {
            auto *o = reinterpret_cast<int16_t *>(dst); // DAC: full 16-bit, MSB-weighted
            if (s->format == SOAPY_SDR_CS16) std::memcpy(o, src, n * 4);
            else if (s->format == SOAPY_SDR_CS8) { auto *p = static_cast<const int8_t *>(src); for (size_t i = 0; i < 2 * n; i++) o[i] = static_cast<int16_t>(p[i] * 256); }
            else { auto *p = static_cast<const float *>(src); for (size_t i = 0; i < 2 * n; i++) o[i] = clip16(p[i]); }
        }
    }

    static void LIBUSB_CALL rxDone(libusb_transfer *t) {
        auto *s = static_cast<ZCStream *>(t->user_data);
        s->inflight--;
        if (t->status == LIBUSB_TRANSFER_COMPLETED && t->actual_length > 0) {
            size_t len = (size_t)t->actual_length;
            {
                std::lock_guard<std::mutex> l(s->m);
                if (s->rcap - s->rcount >= len) {              // room in the ring
                    size_t first = std::min(len, s->rcap - s->rhead);
                    std::memcpy(s->ring.data() + s->rhead, t->buffer, first);
                    if (len > first) std::memcpy(s->ring.data(), t->buffer + first, len - first);
                    s->rhead = (s->rhead + len) % s->rcap; s->rcount += len;
                    s->cv.notify_one();
                } else {
                    s->rxoverrun = true;                        // host consumer too slow -> drop this block
                }
            }
        }
        if (s->active && t->status != LIBUSB_TRANSFER_CANCELLED) { s->inflight++; if (libusb_submit_transfer(t)) s->inflight--; }
    }
    static void LIBUSB_CALL txDone(libusb_transfer *t) {
        auto *s = static_cast<ZCStream *>(t->user_data);
        s->inflight--;
        std::lock_guard<std::mutex> l(s->m);
        for (auto &x : s->xfers) if (x.t == t) { s->ready.push_back(&x); break; }
        s->cv.notify_one();
    }

    libusb_context *ctx_ = nullptr;
    libusb_device_handle *h_ = nullptr;
    int if_ = -1;
    uint8_t epIn_ = 0, epOut_ = 0;
    std::string serial_;
    std::thread ev_;
    std::atomic<bool> evRun_{false};
    std::mutex ctrlM_;
    double loMin_ = 0, loMax_ = 0;
    std::string ant_[2];
    bool agc_ = false;
    int agcMode_ = 1;
    ZCStream *rx_ = nullptr, *tx_ = nullptr;
};

SoapySDR::KwargsList findNanoZC(const SoapySDR::Kwargs &) {
    SoapySDR::KwargsList out;
    libusb_context *ctx;
    if (libusb_init(&ctx)) return out;
    libusb_device **list;
    ssize_t n = libusb_get_device_list(ctx, &list);
    for (ssize_t i = 0; i < n; i++) {
        int ifn; uint8_t in, outEp;
        if (!NanoZC::probe(list[i], ifn, in, outEp)) continue;
        SoapySDR::Kwargs k{{"driver", "nanozc"}, {"label", "PlutoSDR Nano zero-copy"}};
        libusb_device_handle *h;
        if (!libusb_open(list[i], &h)) { k["serial"] = NanoZC::serialOf(h, list[i]); libusb_close(h); }
        out.push_back(k);
    }
    libusb_free_device_list(list, 1);
    libusb_exit(ctx);
    return out;
}

SoapySDR::Device *makeNanoZC(const SoapySDR::Kwargs &args) { return new NanoZC(args); }

} // namespace

static SoapySDR::Registry registerNanoZC("nanozc", &findNanoZC, &makeNanoZC, SOAPY_SDR_ABI_VERSION);
