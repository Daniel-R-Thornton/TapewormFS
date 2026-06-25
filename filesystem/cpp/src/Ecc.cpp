#include "tapefs/Ecc.hpp"
#include <cstring>
#include <array>

namespace tapefs {

namespace {

// GF(256) tables built at startup
struct GfTables {
    std::array<uint8_t, 256> log{};
    std::array<uint8_t, 256> alog{};

    GfTables() {
        uint16_t x = 1;
        for (int i = 0; i < 255; i++) {
            alog[i] = static_cast<uint8_t>(x);
            log[x]  = static_cast<uint8_t>(i);
            x <<= 1;
            if (x & 0x100) x ^= 0x11D;
        }
        log[0] = 0;
        alog[255] = alog[0];
    }

    uint8_t mul(uint8_t a, uint8_t b) const {
        if (a == 0 || b == 0) return 0;
        return alog[(log[a] + log[b]) % 255];
    }
};

const GfTables& gf() {
    static GfTables t;
    return t;
}

// Generator polynomial coefficients (g[0..16], g[16]=1)
std::array<uint8_t, 17> buildGenPoly() {
    const auto& F = gf();
    std::array<uint8_t, 17> poly{};
    poly[0] = 1;

    for (int i = 0; i < 16; i++) {
        uint8_t a = F.alog[i];
        std::array<uint8_t, 17> next{};
        for (int j = 0; j <= i; j++) {
            next[j + 1] ^= poly[j];
            next[j]   ^= F.mul(poly[j], a);
        }
        poly = next;
    }
    return poly;
}

const auto& genPoly() {
    static auto g = buildGenPoly();
    return g;
}

} // namespace

// ---- Encoder ------------------------------------------------------ //

Ecc::Parity Ecc::encode(const Data& data) {
    const auto& F = gf();
    const auto& g = genPoly();

    std::array<uint8_t, 255> dividend{};
    std::copy(data.begin(), data.end(), dividend.begin());

    for (int i = 0; i < 239; i++) {
        if (dividend[i]) {
            uint8_t fb = dividend[i];
            for (int j = 0; j <= 16; j++) {
                dividend[16 + i - j] ^= F.mul(fb, g[j]);
            }
        }
    }

    Parity parity{};
    std::copy(dividend.begin() + 239, dividend.end(), parity.begin());
    return parity;
}

// ---- Decoder ------------------------------------------------------ //

static int syndromes(const uint8_t* cw, uint8_t* syn) {
    const auto& F = gf();
    int nerr = 0;
    for (int i = 0; i < 16; i++) {
        uint8_t s = 0;
        for (int j = 0; j < 255; j++) {
            s = F.mul(s, F.alog[i]) ^ cw[j];
        }
        syn[i] = s;
        if (s) nerr++;
    }
    return nerr;
}

Status Ecc::decode(Data& data, const Parity& parity) {
    uint8_t cw[255];
    std::copy(data.begin(), data.end(), cw);
    std::copy(parity.begin(), parity.end(), cw + 239);

    uint8_t syn[16];
    int nerr = syndromes(cw, syn);
    if (nerr == 0) return {true};

    // BM + Chien + Forney — under development
    return {false, "correction not yet implemented"};
}

} // namespace tapefs
