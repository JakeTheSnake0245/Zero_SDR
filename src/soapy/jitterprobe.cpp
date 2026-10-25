// jitterprobe: measure how smoothly the driver delivers RX samples — the thing that starves a real-time
// demod (choppy audio). Reads CF32 in SDR++-sized blocks, records arrival times, and computes the
// jitter buffer a real-time consumer at exactly the sample rate would need to never starve.
// usage: jitterprobe RATE_HZ SECS [block_samples]
#include <SoapySDR/Device.hpp>
#include <SoapySDR/Formats.hpp>
#include <SoapySDR/Errors.hpp>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <thread>
#include <cstdlib>

using clk = std::chrono::steady_clock;
static double ms(clk::duration d) { return std::chrono::duration<double, std::milli>(d).count(); }

int main(int argc, char **argv) {
    double rate = atof(argv[1]); double secs = argc > 2 ? atof(argv[2]) : 20;
    size_t blk = argc > 3 ? atoll(argv[3]) : (size_t)(rate / 200); double stall_ms = getenv("STALL_MS") ? atof(getenv("STALL_MS")) : 0;
    auto *d = SoapySDR::Device::make("driver=nanozc");
    d->setSampleRate(SOAPY_SDR_RX, 0, rate);
    d->setFrequency(SOAPY_SDR_RX, 0, 100e6);
    auto *s = d->setupStream(SOAPY_SDR_RX, SOAPY_SDR_CF32, {0}, {{"bits", "8"}});
    std::vector<std::complex<float>> buf(blk);
    void *bp[] = {buf.data()};
    d->activateStream(s);
    // discard 0.5 s of warm-up
    auto warm = clk::now();
    while (ms(clk::now() - warm) < 500) { int f; long long t; d->readStream(s, bp, blk, f, t, 200000); }

    long long got = 0, ovf = 0, reads = 0;
    double worstGap = 0, needBufMs = 0, maxDeficitSamp = 0;
    auto t0 = clk::now(), prev = t0;
    while (ms(clk::now() - t0) < secs * 1000) {
        int flags; long long tns;
        int r = d->readStream(s, bp, blk, flags, tns, 500000);
        auto now = clk::now();
        if (stall_ms > 0 && reads % 200 == 0 && reads > 0) std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(stall_ms)); // simulate a periodic DSP/GUI hitch
        if (r == SOAPY_SDR_OVERFLOW) { ovf++; continue; }
        if (r <= 0) continue;
        double gap = ms(now - prev); prev = now;
        if (gap > worstGap) worstGap = gap;
        got += r; reads++;
        // a real-time sink consumes rate*elapsed; the deficit it must cover from buffer:
        double elapsed_s = std::chrono::duration<double>(now - t0).count();
        double deficit = rate * elapsed_s - got; // samples the sink wanted but we haven't delivered yet
        if (deficit > maxDeficitSamp) { maxDeficitSamp = deficit; needBufMs = deficit / rate * 1000; }
    }
    double el = std::chrono::duration<double>(clk::now() - t0).count();
    d->deactivateStream(s); d->closeStream(s);
    printf("rate %5.1f MS/s | block %6zu samp (%.2f ms) | reads %lld | delivered %.3f MS/s | overflow-returns %lld | worst inter-read gap %.1f ms | jitter-buffer needed %.1f ms\n",
           rate / 1e6, blk, blk / rate * 1e3, reads, got / el / 1e6, ovf, worstGap, needBufMs);
    SoapySDR::Device::unmake(d);
    return 0;
}
