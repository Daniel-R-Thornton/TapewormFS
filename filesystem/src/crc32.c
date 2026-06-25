#include "tapefs.h"

/* CRC-32 (ISO 3309 / IEEE 802.3): poly = 0xEDB88320 (reflected) */
static uint32_t crc32_tab[256];
static int      crc32_tab_init = 0;

static void crc32_init_table(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (int j = 0; j < 8; j++) {
            if (crc & 1)
                crc = (crc >> 1) ^ 0xEDB88320UL;
            else
                crc >>= 1;
        }
        crc32_tab[i] = crc;
    }
    crc32_tab_init = 1;
}

uint32_t tapefs_crc32(const uint8_t *buf, uint32_t len) {
    if (!crc32_tab_init) crc32_init_table();

    uint32_t crc = 0xFFFFFFFFUL;
    for (uint32_t i = 0; i < len; i++) {
        crc = crc32_tab[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFUL;
}
