#include "tapefs.h"
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Reed-Solomon RS(255, 239) over GF(256)                            */
/*  Primitive polynomial: x^8 + x^4 + x^3 + x^2 + 1 (0x11D)          */
/*  Generator: g(x) = (x - a^0)(x - a^1)...(x - a^(2T-1)), a=2       */
/*  Corrects up to T=8 byte errors.                                   */
/* ------------------------------------------------------------------ */

#define RS_N       255
#define RS_K       239
#define RS_2T      16
#define RS_T       8
#define RS_PRIM    0x11D

static uint8_t log_tbl[256];
static uint8_t alog_tbl[256];

static void gf_init(void) {
    /* Guard via array check — compiler can't optimise this away */
    if (alog_tbl[1] == 2) return;
    uint16_t x = 1;
    for (int i = 0; i < 255; i++) {
        alog_tbl[i] = (uint8_t)x;
        log_tbl[x]  = (uint8_t)i;
        x <<= 1;
        if (x & 0x100) x ^= RS_PRIM;
    }
    log_tbl[0] = 0;
    alog_tbl[255] = alog_tbl[0];
}

static uint8_t gf_mul(uint8_t a, uint8_t b) {
    if (a == 0 || b == 0) return 0;
    return alog_tbl[(log_tbl[a] + log_tbl[b]) % 255];
}

static uint8_t gf_div(uint8_t a, uint8_t b) {
    if (a == 0 || b == 0) return 0;
    int d = (int)log_tbl[a] - (int)log_tbl[b];
    if (d < 0) d += 255;
    return alog_tbl[d];
}

/* Generate generator polynomial coefficients into g[0..2T]. */
static void gen_poly(uint8_t *g) {
    uint8_t poly[RS_2T + 1];
    memset(poly, 0, RS_2T + 1);
    poly[0] = 1;

    for (int i = 0; i < RS_2T; i++) {
        uint8_t a = alog_tbl[i];
        for (int j = i; j >= 0; j--) poly[j + 1] = poly[j];
        poly[0] = 0;
        for (int j = 0; j <= i; j++)
            poly[j] ^= gf_mul(poly[j + 1], a);
    }
    memcpy(g, poly, RS_2T + 1);
}

void tapefs_rs_encode(const uint8_t data[RS_K], uint8_t parity[RS_2T]) {
    gf_init();

    uint8_t gen[RS_2T + 1];
    gen_poly(gen);

    /* Polynomial division: dividend = data * x^(2T), divide by g(x).
     * data[0] = coefficient of x^(K-1) (highest degree).
     * Cancel terms from highest degree (index 0) downwards. */
    uint8_t dividend[RS_N];
    memset(dividend, 0, RS_N);
    memcpy(dividend, data, RS_K);

    for (int i = 0; i < RS_K; i++) {
        if (dividend[i] != 0) {
            uint8_t fb = dividend[i];
            /* Subtract fb * g(x) * x^(K-1-i), which aligns gen[2T]=1
             * with the term at dividend[i] (coefficient at x^(N-1-i)). */
            for (int j = 0; j <= RS_2T; j++) {
                dividend[RS_2T + i - j] ^= gf_mul(fb, gen[j]);
            }
        }
    }

    memcpy(parity, dividend + RS_K, RS_2T);
}

/* ------------------------------------------------------------------ */
/*  Decoder: syndrome + BM + Chien + Forney                           */
/* ------------------------------------------------------------------ */

static int syndromes(const uint8_t *cw, int n, uint8_t *syn) {
    int nerr = 0;
    for (int i = 0; i < RS_2T; i++) {
        uint8_t s = 0;
        for (int j = 0; j < n; j++)
            s = gf_mul(s, alog_tbl[i]) ^ cw[j];
        syn[i] = s;
        if (s != 0) nerr++;
    }
    return nerr;
}

static int berlekamp(const uint8_t *syn, uint8_t *lambda, uint8_t *omega) {
    uint8_t b[RS_2T + 1], t[RS_2T + 1];
    memset(lambda, 0, RS_2T + 1);
    memset(b, 0, RS_2T + 1);
    memset(omega, 0, RS_2T + 1);
    lambda[0] = 1;
    b[0] = 1;

    int L = 0, m = 1;

    for (int r = 0; r < RS_2T; r++) {
        uint8_t d = 0;
        for (int j = 0; j <= L; j++)
            d ^= gf_mul(lambda[j], syn[r - j]);

        if (d == 0) {
            m++;
        } else {
            memcpy(t, lambda, (RS_2T + 1) * sizeof(uint8_t));
            for (int j = 0; j <= RS_2T - m; j++)
                if (b[j]) lambda[j + m] ^= gf_mul(d, b[j]);

            if (2 * L <= r) {
                L = r + 1 - L;
                memcpy(b, t, (RS_2T + 1) * sizeof(uint8_t));
                uint8_t dinv = gf_div(1, d);
                for (int j = 0; j <= RS_2T; j++)
                    b[j] = gf_mul(b[j], dinv);
                m = 1;
            } else {
                m++;
            }
        }
    }

    /* Omega(x) = S(x) * Lambda(x) mod x^(2T) */
    for (int i = 0; i < RS_2T; i++) {
        omega[i] = 0;
        for (int j = 0; j <= i; j++)
            omega[i] ^= gf_mul(syn[j], lambda[i - j]);
    }

    return L;
}

static int chien(const uint8_t *lambda, int degree, uint8_t *pos) {
    int cnt = 0;
    for (int i = 1; i <= RS_N; i++) {
        uint8_t val = 0, apow = 1;
        for (int j = 0; j <= degree; j++) {
            val ^= gf_mul(lambda[j], apow);
            apow = gf_mul(apow, alog_tbl[1]);
        }
        if (val == 0) {
            pos[cnt++] = (uint8_t)(RS_N - i);
            if (cnt >= degree) break;
        }
    }
    return cnt;
}

static void forney(const uint8_t *omega, const uint8_t *lambda,
                   int degree, const uint8_t *pos, int cnt,
                   uint8_t *val) {
    for (int i = 0; i < cnt; i++) {
        int X = RS_N - 1 - pos[i];
        uint8_t Xi = alog_tbl[(255 - log_tbl[alog_tbl[X]]) % 255];

        uint8_t omegaX = 0, apow = 1;
        for (int j = 0; j < degree; j++) {
            omegaX ^= gf_mul(omega[j], apow);
            apow = gf_mul(apow, Xi);
        }

        uint8_t lambdaX = 0;
        for (int j = 1; j <= degree; j += 2)
            lambdaX ^= gf_mul(lambda[j], alog_tbl[(j * log_tbl[Xi]) % 255]);

        val[i] = lambdaX ? gf_div(omegaX, lambdaX) : 0;
    }
}

int tapefs_rs_decode(uint8_t data[RS_K], const uint8_t parity[RS_2T]) {
    gf_init();

    uint8_t cw[RS_N];
    memcpy(cw, data, RS_K);
    memcpy(cw + RS_K, parity, RS_2T);

    uint8_t syn[RS_2T];
    int nerr = syndromes(cw, RS_N, syn);
    if (nerr == 0) return TAPEFS_OK;

    uint8_t lambda[RS_2T + 1], omega[RS_2T + 1];
    int degree = berlekamp(syn, lambda, omega);

    if (degree == 0 || degree > RS_T) return TAPEFS_ERR_ECC;

    uint8_t pos[RS_T];
    int found = chien(lambda, degree, pos);
    if (found != degree) return TAPEFS_ERR_ECC;

    uint8_t val[RS_T];
    forney(omega, lambda, degree, pos, found, val);

    for (int i = 0; i < found; i++)
        if (pos[i] < RS_K) data[pos[i]] ^= val[i];

    return TAPEFS_OK;
}
