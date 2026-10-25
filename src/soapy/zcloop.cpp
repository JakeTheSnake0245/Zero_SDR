// zcloop: on-air TX validation by self-reception. Transmits a tone at FREQ+offset and receives it on
// the board's own RX (full duplex), then measures where the tone landed and how strong it is.
// Proves the TX chain actually radiates a correct signal, with no external measurement gear.
//
// Note: TX_LO and RX_LO share the AD9361 40 MHz reference, so the carrier offset cancels in loopback —
// this validates the TX datapath/DDS + radiated output + frequency *consistency*, not absolute LO error
// (that needs an external reference). usage: zcloop FREQ_HZ [--rate R] [--offset O] [--gain G] [--secs S] [--bits B]
#include <SoapySDR/Device.hpp>
#include <SoapySDR/Formats.hpp>
#include <SoapySDR/Errors.hpp>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using cf = std::complex<float>;
static std::atomic<bool> txRun{false};

// single-bin DFT magnitude at frequency f, in dBFS (128 = CS8 full scale)
static double binmag(const std::vector<cf> &x, double rate, double f) {
    size_t N = x.size(); double w = -2 * M_PI * f / rate, re = 0, im = 0;
    for (size_t n = 0; n < N; n++) { double c = std::cos(w * n), s = std::sin(w * n); re += x[n].real() * c - x[n].imag() * s; im += x[n].real() * s + x[n].imag() * c; }
    return 20 * std::log10(std::hypot(re, im) / (N * 128.0) + 1e-12);
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s FREQ_HZ [--rate R] [--offset O] [--gain G] [--secs S] [--bits B]\n", argv[0]); return 2; }
    double freq = atof(argv[1]), rate = 4e6, offset = 200e3, gain = 89.75, secs = 8; int bits = 8; std::string txfile;
    for (int i = 2; i < argc; i++) { std::string a = argv[i]; auto nx = [&] { return atof(argv[++i]); };
        if (a == "--rate") rate = nx(); else if (a == "--offset") offset = nx(); else if (a == "--gain") gain = nx();
        else if (a == "--secs") secs = nx(); else if (a == "--bits") bits = (int)nx(); else if (a == "--file") txfile = argv[++i]; }

    auto *d = SoapySDR::Device::make("driver=nanozc");
    d->setSampleRate(SOAPY_SDR_RX, 0, rate);          // shared RX/TX rate
    d->setFrequency(SOAPY_SDR_RX, 0, freq);
    d->setFrequency(SOAPY_SDR_TX, 0, freq);
    d->setGain(SOAPY_SDR_TX, 0, gain);
    d->setGainMode(SOAPY_SDR_RX, 0, false);
    d->setGain(SOAPY_SDR_RX, 0, 20);                  // modest RX gain; TX leaks strongly into RX on-board
    SoapySDR::Kwargs sa{{"bits", std::to_string(bits)}, {"buffers", "8"}, {"buffer_kib", "256"}};
    auto *tx = d->setupStream(SOAPY_SDR_TX, SOAPY_SDR_CF32, {0}, sa);
    auto *rx = d->setupStream(SOAPY_SDR_RX, SOAPY_SDR_CF32, {0}, sa);

    std::vector<cf> fdata;
    if (!txfile.empty()) { FILE *fp = fopen(txfile.c_str(), "rb"); fseek(fp, 0, SEEK_END); long n = ftell(fp) / 8; fseek(fp, 0, SEEK_SET); fdata.resize(n); (void)!fread(fdata.data(), 8, n, fp); fclose(fp); }
    std::thread tt([&] {
        std::vector<cf> buf(d->getStreamMTU(tx)); double ph = 0, dph = 2 * M_PI * offset / rate; size_t fp = 0;
        d->activateStream(tx); txRun = true;
        while (txRun) { for (auto &s : buf) { if (!fdata.empty()) { s = fdata[fp]; fp = (fp + 1) % fdata.size(); } else { s = std::polar(0.7f, (float)ph); ph = std::fmod(ph + dph, 2 * M_PI); } }
            const void *bp[] = {buf.data()}; int fl = 0; d->writeStream(tx, bp, buf.size(), fl, 0, 200000); }
        d->deactivateStream(tx);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(400)); // let TX come up

    d->activateStream(rx);
    std::vector<cf> cap; cap.reserve((size_t)rate);
    std::vector<cf> blk(65536); void *bp[] = {blk.data()};
    auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < secs && cap.size() < (size_t)(rate / 4)) {
        int fl; long long tns; int r = d->readStream(rx, bp, blk.size(), fl, tns, 300000);
        if (r > 0) cap.insert(cap.end(), blk.begin(), blk.begin() + r);
    }
    txRun = false; tt.join();
    d->deactivateStream(rx);
    std::string unf = d->readSensor("tx_underflows"), temp = d->readSensor("zynq_temperature");
    d->closeStream(tx); d->closeStream(rx); d->writeSetting("tx_arm", "false");

    if (cap.size() < 16384) { printf("FREQ %.6f MHz: too few RX samples (%zu)\n", freq / 1e6, cap.size()); SoapySDR::Device::unmake(d); return 1; }
    cap.resize(65536);
    if (!txfile.empty()) {
        double inband = binmag(cap, rate, -300e3), inband2 = binmag(cap, rate, 300e3), out = binmag(cap, rate, rate * 0.45);
        printf("FREQ %8.3f MHz | REPLAY %s | in-band %+6.1f/%+6.1f dBFS | out-of-band %+6.1f | tx_unf %s | zynq %s C | %s\n",
               freq / 1e6, txfile.c_str(), inband, inband2, out, unf.c_str(), temp.c_str(), (inband > out + 15 || inband2 > out + 15) ? "RADIATED" : "weak");
    } else {
    double tone = binmag(cap, rate, offset), image = binmag(cap, rate, -offset);
    double noise = binmag(cap, rate, offset + rate / 8), dc = binmag(cap, rate, 0);
    printf("FREQ %8.3f MHz | tone %+6.1f dBFS | image %+6.1f (rej %4.1f dB) | DC %+6.1f | noise %+6.1f (SNR %4.1f dB) | tx_unf %s | zynq %s C | %s\n",
           freq / 1e6, tone, image, tone - image, dc, noise, tone - noise, unf.c_str(), temp.c_str(),
           tone - noise > 15 ? "TX OK" : "weak");
    }
    SoapySDR::Device::unmake(d);
    return 0;
}
