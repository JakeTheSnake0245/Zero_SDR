// zctest: functional tests of SoapyNanoZC through the public SoapySDR API only (what SDR++ / GNU Radio use).
// Never arms TX inside limits: TX streaming runs disarmed (DAC path active, LO powered down, -89.75 dB).
// usage: zctest controls | rx RATE BITS SECS [buffer_kib] | tx RATE BITS SECS | duplex RATE BITS SECS | armcheck
#include <SoapySDR/Device.hpp>
#include <SoapySDR/Formats.hpp>
#include <SoapySDR/Errors.hpp>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <atomic>
#include <cstdlib>
static std::string envs(const char *k, const char *d) { const char *v = getenv(k); return v ? v : d; }
#include <vector>

using clk = std::chrono::steady_clock;
static double secs(clk::time_point a) { return std::chrono::duration<double>(clk::now() - a).count(); }

static int controls(SoapySDR::Device *d) {
    int bad = 0;
    auto chk = [&](const char *what, double want, double got, double tol) {
        bool ok = std::fabs(want - got) <= tol;
        printf("  %-28s set %-14.6g got %-14.6g %s\n", what, want, got, ok ? "OK" : "MISMATCH");
        bad += !ok;
    };
    d->setSampleRate(SOAPY_SDR_RX, 0, 21e6); chk("sample rate", 21e6, d->getSampleRate(SOAPY_SDR_RX, 0), 2);
    d->setFrequency(SOAPY_SDR_RX, 0, 915e6); chk("rx LO", 915e6, d->getFrequency(SOAPY_SDR_RX, 0), 5);
    d->setFrequency(SOAPY_SDR_TX, 0, 2450e6); chk("tx LO", 2450e6, d->getFrequency(SOAPY_SDR_TX, 0), 5);
    d->setBandwidth(SOAPY_SDR_RX, 0, 18e6); chk("rx RF bandwidth", 18e6, d->getBandwidth(SOAPY_SDR_RX, 0), 2e5);
    d->setBandwidth(SOAPY_SDR_TX, 0, 18e6); chk("tx RF bandwidth", 18e6, d->getBandwidth(SOAPY_SDR_TX, 0), 2e5);
    d->setGainMode(SOAPY_SDR_RX, 0, false); d->setGain(SOAPY_SDR_RX, 0, 40); chk("rx gain (manual)", 40, d->getGain(SOAPY_SDR_RX, 0), 1);
    d->setGain(SOAPY_SDR_TX, 0, 30); chk("tx gain (stored; disarmed)", 30, d->getGain(SOAPY_SDR_TX, 0), 0.25);
    for (auto a : d->listAntennas(SOAPY_SDR_RX, 0)) { d->setAntenna(SOAPY_SDR_RX, 0, a); printf("  rx antenna %-15s OK\n", a.c_str()); }
    d->setAntenna(SOAPY_SDR_RX, 0, "A_BALANCED");
    for (auto a : d->listAntennas(SOAPY_SDR_TX, 0)) { d->setAntenna(SOAPY_SDR_TX, 0, a); printf("  tx antenna %-15s OK\n", a.c_str()); }
    d->setAntenna(SOAPY_SDR_TX, 0, "A");
    for (auto m : {"slow", "fast", "hybrid"}) { d->writeSetting("agc_mode", m); d->setGainMode(SOAPY_SDR_RX, 0, true); printf("  rx AGC %-8s on: %s\n", m, d->getGainMode(SOAPY_SDR_RX, 0) ? "OK" : "FAIL"); }
    d->setGainMode(SOAPY_SDR_RX, 0, false);
    for (auto k : {"quadrature_tracking", "rf_dc_tracking", "bb_dc_tracking"}) { d->writeSetting(k, "true"); printf("  %-22s true OK\n", k); }
    printf("  sensors: zynq %s C, ad936x %s C, rssi %s dB, armed %s\n", d->readSensor("zynq_temperature").c_str(), d->readSensor("temperature").c_str(), d->readSensor("rssi").c_str(), d->readSensor("tx_armed").c_str());
    return bad;
}

static int rx(SoapySDR::Device *d, double rate, int bits, double dur, const char *bkib) {
    d->setSampleRate(SOAPY_SDR_RX, 0, rate);
    SoapySDR::Kwargs a{{"bits", std::to_string(bits)}, {"buffers", envs("ZC_NBUF", "32")}, {"buffer_kib", envs("ZC_BKIB", bkib)}};
    auto *s = d->setupStream(SOAPY_SDR_RX, SOAPY_SDR_CF32, {0}, a);
    std::vector<std::complex<float>> buf(d->getStreamMTU(s));
    void *bp[] = {buf.data()};
    d->activateStream(s);
    long long n = 0, ovf = 0, to = 0, firstT = -1, lastT = 0; double pwr = 0; long long pn = 0;
    auto t0 = clk::now();
    while (secs(t0) < dur) {
        int flags; long long tns;
        int r = d->readStream(s, bp, buf.size(), flags, tns, 200000);
        if (r == SOAPY_SDR_OVERFLOW) { ovf++; continue; }
        if (r == SOAPY_SDR_TIMEOUT) { to++; continue; }
        if (r < 0) { printf("  readStream error %d\n", r); break; }
        if (firstT < 0) firstT = tns;
        lastT = tns; n += r;
        if (pn < 1000000) for (int i = 0; i < r; i++) { pwr += std::norm(buf[i]); pn++; }
    }
    double el = secs(t0);
    d->deactivateStream(s); d->closeStream(s);
    printf("  RX %d-bit CF32 @ %.2f MS/s, %s KiB bufs: %lld samples in %.1f s = %.3f MS/s, overflows %lld, timeouts %lld, mean |x|^2 %.2e, timestamp span %.2f s\n",
           bits, rate / 1e6, bkib, n, el, n / el / 1e6, ovf, to, pwr / std::max(1LL, pn), (lastT - firstT) / 1e9);
    return ovf || to;
}

static std::atomic<bool> txRun{false};
static int tx(SoapySDR::Device *d, double rate, int bits, double dur, bool quiet = false) {
    d->setSampleRate(SOAPY_SDR_TX, 0, rate);
    if (d->readSensor("tx_armed") != "false") { printf("  refusing: TX is armed\n"); return 1; }
    SoapySDR::Kwargs a{{"bits", std::to_string(bits)}, {"buffers", envs("ZC_NBUF", "32")}, {"buffer_kib", envs("ZC_BKIB", "64")}};
    auto *s = d->setupStream(SOAPY_SDR_TX, SOAPY_SDR_CF32, {0}, a);
    std::vector<std::complex<float>> buf(d->getStreamMTU(s));
    for (size_t i = 0; i < buf.size(); i++) buf[i] = 0.5f * std::polar(1.0f, float(2 * M_PI * i / 16)); // fs/16 tone (disarmed: no RF)
    const void *bp[] = {buf.data()};
    d->activateStream(s);
    long long n = 0, to = 0;
    auto t0 = clk::now();
    txRun = true;
    while (secs(t0) < dur) {
        int flags = 0;
        int r = d->writeStream(s, bp, buf.size(), flags, 0, 200000);
        if (r == SOAPY_SDR_TIMEOUT) { to++; continue; }
        if (r < 0) { printf("  writeStream error %d\n", r); break; }
        n += r;
        size_t m; int f; long long t;
        if (n % (buf.size() * 64) == 0 && d->readStreamStatus(s, m, f, t, 0) == SOAPY_SDR_UNDERFLOW) {}
    }
    double el = secs(t0);
    unsigned long long bdunf = std::stoull(d->readSensor("tx_underflows"));
    std::string armed = d->readSensor("tx_armed");
    d->deactivateStream(s); d->closeStream(s);
    txRun = false;
    if (!quiet || to || bdunf) printf("  TX %d-bit CF32 @ %.2f MS/s (DISARMED): %lld samples in %.1f s = %.3f MS/s, write timeouts %lld, board underflows %llu, armed=%s\n",
           bits, rate / 1e6, n, el, n / el / 1e6, to, bdunf, armed.c_str());
    return to > 0 || bdunf > 1;
}

static int armcheck(SoapySDR::Device *d) {
    int bad = 0;
    double lo = d->getFrequency(SOAPY_SDR_TX, 0);
    std::string amin = d->readSetting("tx_allow_min_hz"), amax = d->readSetting("tx_allow_max_hz");
    printf("  allow-list %s .. %s Hz, TX LO %.0f\n", amin.c_str(), amax.c_str(), lo);
    d->writeSetting("tx_allow_max_hz", std::to_string(lo - 100e6));     // LO now outside the allow-list
    try { d->writeSetting("tx_arm", "true"); printf("  arm outside allow-list: ACCEPTED (BAD)\n"); bad++; }
    catch (const std::exception &e) { printf("  arm outside allow-list: refused OK (%s)\n", e.what()); }
    printf("  tx_armed after refusal: %s\n", d->readSensor("tx_armed").c_str());
    if (d->readSensor("tx_armed") != "false") bad++;
    d->writeSetting("tx_allow_max_hz", amax);                              // restore
    d->writeSetting("tx_atten_ceiling_db", "-60");                         // ceiling: at least 60 dB attenuation
    d->setGain(SOAPY_SDR_TX, 0, 89.75);                                    // request 0 dB attenuation (> ceiling)
    try { d->writeSetting("tx_arm", "true"); printf("  arm above power ceiling: ACCEPTED (BAD)\n"); bad++; }
    catch (const std::exception &e) { printf("  arm above power ceiling: refused OK (%s)\n", e.what()); }
    if (d->readSensor("tx_armed") != "false") bad++;
    d->writeSetting("tx_atten_ceiling_db", "0");
    d->setGain(SOAPY_SDR_TX, 0, 0);
    printf("  final tx_armed: %s\n", d->readSensor("tx_armed").c_str());
    return bad;
}

int main(int argc, char **argv) {
    std::string mode = argc > 1 ? argv[1] : "controls";
    auto *d = SoapySDR::Device::make("driver=nanozc");
    int bad = 0;
    if (mode == "controls") bad = controls(d);
    else if (mode == "rx") bad = rx(d, atof(argv[2]), atoi(argv[3]), atof(argv[4]), argc > 5 ? argv[5] : "64");
    else if (mode == "tx") bad = tx(d, atof(argv[2]), atoi(argv[3]), atof(argv[4]));
    else if (mode == "armcheck") bad = armcheck(d);
    else if (mode == "duplex_rxfirst") {
        double r = atof(argv[2]); int b = atoi(argv[3]); double s = atof(argv[4]);
        int rxbad = 0;
        std::thread t([&] { rxbad = rx(d, r, b, s, "64"); });
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        bad = tx(d, r, b, s - 0.6, false);
        t.join(); bad |= rxbad;
    }
    else if (mode == "duplex") {
        double r = atof(argv[2]); int b = atoi(argv[3]); double s = atof(argv[4]);
        int txbad = 0;
        std::thread t([&] { txbad = tx(d, r, b, s, false); });
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        bad = rx(d, r, b, s - 0.6, "64");
        t.join(); bad |= txbad;
    }
    else { fprintf(stderr, "unknown mode '%s'\n", mode.c_str()); bad = 2; }
    printf("%s: %s\n", mode.c_str(), bad ? "FAIL" : "PASS");
    SoapySDR::Device::unmake(d);
    return bad ? 1 : 0;
}
