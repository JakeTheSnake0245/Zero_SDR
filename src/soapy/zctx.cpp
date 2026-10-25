// zctx: transmit through SoapyNanoZC (zero-copy TX path).
// usage: zctx FREQ_HZ [--rate R] [--gain G] [--tone OFFSET_HZ | --cw | --file IQ.cf32] [--amp A] [--secs S] [--bits 8|16] [--leave-on]
//   gain G: 0..89.75 dB (89.75 = 0 dB attenuation = maximum output, roughly +5..+7 dBm on the Nano)
//   default: 2.5 MS/s, tone at +100 kHz, amp 0.7, 10 s, 16-bit, TX turned off at the end
#include <SoapySDR/Device.hpp>
#include <SoapySDR/Formats.hpp>
#include <SoapySDR/Errors.hpp>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s FREQ_HZ [--rate R] [--gain G] [--tone OFF | --cw | --file F] [--amp A] [--secs S] [--bits 8|16] [--leave-on]\n", argv[0]); return 2; }
    double freq = atof(argv[1]), rate = 2.5e6, gain = 89.75, tone = 100e3, amp = 0.7, secs = 10;
    int bits = 16; bool cw = false, leaveOn = false; std::string file;
    for (int i = 2; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&] { return i + 1 < argc ? argv[++i] : (char *)"0"; };
        if (a == "--rate") rate = atof(next());
        else if (a == "--gain") gain = atof(next());
        else if (a == "--tone") tone = atof(next());
        else if (a == "--cw") cw = true;
        else if (a == "--file") file = next();
        else if (a == "--amp") amp = atof(next());
        else if (a == "--secs") secs = atof(next());
        else if (a == "--bits") bits = atoi(next());
        else if (a == "--leave-on") leaveOn = true;
        else { fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
    }
    auto *d = SoapySDR::Device::make("driver=nanozc");
    d->setSampleRate(SOAPY_SDR_TX, 0, rate);
    d->setFrequency(SOAPY_SDR_TX, 0, freq);
    d->setGain(SOAPY_SDR_TX, 0, gain);
    auto *s = d->setupStream(SOAPY_SDR_TX, SOAPY_SDR_CF32, {0}, {{"bits", std::to_string(bits)}, {"buffers", "32"}, {"buffer_kib", "64"}});
    std::vector<std::complex<float>> buf(d->getStreamMTU(s));
    std::vector<std::complex<float>> filedata;
    if (!file.empty()) {
        std::ifstream f(file, std::ios::binary);
        f.seekg(0, std::ios::end); size_t n = f.tellg() / sizeof(std::complex<float>); f.seekg(0);
        filedata.resize(n); f.read(reinterpret_cast<char *>(filedata.data()), n * sizeof(std::complex<float>));
        if (filedata.empty()) { fprintf(stderr, "empty file\n"); return 1; }
    }
    double ph = 0, dph = 2 * M_PI * tone / rate; size_t fpos = 0;
    printf("TX %.6f MHz%s, rate %.3f MS/s, gain %.2f dB (atten %.2f dB), %s, %d-bit, %.1f s\n", freq / 1e6,
           cw ? " (CW)" : file.empty() ? "" : " (file)", rate / 1e6, gain, gain - 89.75,
           cw ? "carrier at LO" : file.empty() ? ("tone at +" + std::to_string((int)tone) + " Hz").c_str() : file.c_str(), bits, secs);
    d->activateStream(s);
    long long sent = 0;
    auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < secs) {
        for (auto &x : buf) {
            if (!filedata.empty()) { x = filedata[fpos]; fpos = (fpos + 1) % filedata.size(); }
            else if (cw) x = {static_cast<float>(amp), 0.f};
            else { x = std::polar(static_cast<float>(amp), static_cast<float>(ph)); ph = std::fmod(ph + dph, 2 * M_PI); }
        }
        const void *bp[] = {buf.data()};
        int flags = 0;
        int r = d->writeStream(s, bp, buf.size(), flags, 0, 500000);
        if (r > 0) sent += r;
        else if (r != SOAPY_SDR_TIMEOUT) { fprintf(stderr, "writeStream error %d\n", r); break; }
    }
    double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    printf("sent %lld samples in %.1f s (%.3f MS/s), board underflows %s, armed %s, zynq %s C\n", sent, el, sent / el / 1e6,
           d->readSensor("tx_underflows").c_str(), d->readSensor("tx_armed").c_str(), d->readSensor("zynq_temperature").c_str());
    d->deactivateStream(s);
    d->closeStream(s);
    if (!leaveOn) { d->writeSetting("tx_arm", "false"); printf("TX off (LO powered down, -89.75 dB)\n"); }
    else printf("TX left ON (--leave-on): LO at %.6f MHz still powered\n", freq / 1e6);
    SoapySDR::Device::unmake(d);
    return 0;
}
