// Independent TA-Lib reference check.
//
// Reads OHLCV as CSV on stdin (open,high,low,close,volume — one bar per line,
// oldest first) and prints the LAST value of a representative function from
// every TA-Lib group, computed by calling the vendored TA-Lib 0.7.1 directly.
//
// This exists to answer one question with evidence rather than trust: are the
// numbers AltTech stores the numbers TA-Lib produces for the same bars? The
// service reaches TA-Lib through the abstract interface (TA_CallFunc over a
// parameter matrix); this tool calls the concrete functions instead, so a fault
// in the abstract layer cannot hide behind itself.

#include "ta_libc.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

void emit(const char* name, int out_beg, int out_n, const double* buf) {
    if (out_n <= 0) { std::printf("%s=NA\n", name); return; }
    (void)out_beg;
    std::printf("%s=%.10g\n", name, buf[out_n - 1]);
}

void emit_i(const char* name, int out_beg, int out_n, const int* buf) {
    if (out_n <= 0) { std::printf("%s=NA\n", name); return; }
    (void)out_beg;
    std::printf("%s=%d\n", name, buf[out_n - 1]);
}

} // namespace

int main() {
    std::vector<double> o, h, l, c, v;
    char line[512];
    while (std::fgets(line, sizeof(line), stdin)) {
        double a, b, d, e, f;
        if (std::sscanf(line, "%lf,%lf,%lf,%lf,%lf", &a, &b, &d, &e, &f) == 5) {
            o.push_back(a); h.push_back(b); l.push_back(d); c.push_back(e); v.push_back(f);
        }
    }
    const int n = static_cast<int>(c.size());
    if (n < 60) { std::fprintf(stderr, "need >= 60 bars, got %d\n", n); return 2; }
    if (TA_Initialize() != TA_SUCCESS) { std::fprintf(stderr, "TA_Initialize failed\n"); return 3; }

    std::printf("bars=%d\n", n);
    std::vector<double> out(static_cast<size_t>(n));
    std::vector<int> outi(static_cast<size_t>(n));
    int beg = 0, cnt = 0;

    TA_ATR(0, n - 1, h.data(), l.data(), c.data(), 14, &beg, &cnt, out.data());
    emit("ATR_14", beg, cnt, out.data());
    TA_NATR(0, n - 1, h.data(), l.data(), c.data(), 14, &beg, &cnt, out.data());
    emit("NATR_14", beg, cnt, out.data());
    TA_ADX(0, n - 1, h.data(), l.data(), c.data(), 14, &beg, &cnt, out.data());
    emit("ADX_14", beg, cnt, out.data());
    TA_CCI(0, n - 1, h.data(), l.data(), c.data(), 14, &beg, &cnt, out.data());
    emit("CCI_14", beg, cnt, out.data());
    TA_MFI(0, n - 1, h.data(), l.data(), c.data(), v.data(), 14, &beg, &cnt, out.data());
    emit("MFI_14", beg, cnt, out.data());
    TA_RSI(0, n - 1, c.data(), 14, &beg, &cnt, out.data());
    emit("RSI_14", beg, cnt, out.data());
    TA_WILLR(0, n - 1, h.data(), l.data(), c.data(), 14, &beg, &cnt, out.data());
    emit("WILLR_14", beg, cnt, out.data());
    TA_HT_DCPERIOD(0, n - 1, c.data(), &beg, &cnt, out.data());
    emit("HT_DCPERIOD", beg, cnt, out.data());
    TA_AD(0, n - 1, h.data(), l.data(), c.data(), v.data(), &beg, &cnt, out.data());
    emit("AD", beg, cnt, out.data());
    TA_OBV(0, n - 1, c.data(), v.data(), &beg, &cnt, out.data());
    emit("OBV", beg, cnt, out.data());
    TA_CDLDOJI(0, n - 1, o.data(), h.data(), l.data(), c.data(), &beg, &cnt, outi.data());
    emit_i("CDLDOJI", beg, cnt, outi.data());
    TA_LINEARREG(0, n - 1, c.data(), 14, &beg, &cnt, out.data());
    emit("LINEARREG_14", beg, cnt, out.data());
    TA_STDDEV(0, n - 1, c.data(), 5, 1.0, &beg, &cnt, out.data());
    emit("STDDEV_5", beg, cnt, out.data());
    TA_AVGPRICE(0, n - 1, o.data(), h.data(), l.data(), c.data(), &beg, &cnt, out.data());
    emit("AVGPRICE", beg, cnt, out.data());
    TA_TYPPRICE(0, n - 1, h.data(), l.data(), c.data(), &beg, &cnt, out.data());
    emit("TYPPRICE", beg, cnt, out.data());
    TA_SMA(0, n - 1, c.data(), 5, &beg, &cnt, out.data());
    emit("SMA_5", beg, cnt, out.data());

    TA_Shutdown();
    return 0;
}
