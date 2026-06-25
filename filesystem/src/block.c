#include "tapefs.h"
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Block Serialisation                                                */
/* ------------------------------------------------------------------ */

/* Wire format:
 *   [0]     type       (1 byte)
 *   [1..4]  seq_no     (4 bytes, little-endian)
 *   [5..]   payload    (data_len bytes)
 *   [..]    rs_parity  (TAPEFS_ECC_PARITY_BYTES bytes)
 *   [..]    crc32      (4 bytes, little-endian)
 *
 * The block is processed in 239-byte chunks for RS(255,239).
 * Each chunk produces 16 parity bytes appended before CRC.
 */

tapefs_raw_block_t tapefs_block_serialise(const tapefs_block_t *block) {
    tapefs_raw_block_t raw;
    uint8_t *buf = raw.bytes;
    uint32_t pos = 0;

    /* Type */
    buf[pos++] = block->type;

    /* Sequence number (little-endian) */
    buf[pos++] = (uint8_t)(block->seq_no & 0xFF);
    buf[pos++] = (uint8_t)((block->seq_no >> 8) & 0xFF);
    buf[pos++] = (uint8_t)((block->seq_no >> 16) & 0xFF);
    buf[pos++] = (uint8_t)((block->seq_no >> 24) & 0xFF);

    /* Payload data */
    uint32_t data_len = block->data_len;
    if (data_len > TAPEFS_BLOCK_DATA_MAX)
        data_len = TAPEFS_BLOCK_DATA_MAX;
    memcpy(buf + pos, block->data, data_len);
    pos += data_len;

    /* RS encode in 239-byte chunks */
    uint32_t total_bytes = TAPEFS_BLOCK_HEADER + data_len;
    uint32_t offset = 0;

    while (offset < total_bytes) {
        uint32_t chunk_size = total_bytes - offset;
        if (chunk_size > 239) chunk_size = 239;

        uint8_t rs_buf[239];
        uint8_t rs_parity[16];
        memset(rs_buf, 0, 239);
        memcpy(rs_buf, buf + offset, chunk_size);

        tapefs_rs_encode(rs_buf, rs_parity);
        memcpy(buf + pos, rs_parity, 16);
        pos += 16;

        offset += chunk_size;
    }

    /* Append CRC-32 */
    uint32_t crc = tapefs_crc32(buf, pos);
    buf[pos++] = (uint8_t)(crc & 0xFF);
    buf[pos++] = (uint8_t)((crc >> 8) & 0xFF);
    buf[pos++] = (uint8_t)((crc >> 16) & 0xFF);
    buf[pos++] = (uint8_t)((crc >> 24) & 0xFF);

    raw.len = pos;
    return raw;
}

int tapefs_block_deserialise(const tapefs_raw_block_t *raw,
                             tapefs_block_t *block) {
    const uint8_t *buf = raw->bytes;
    uint32_t len = raw->len;

    if (len < TAPEFS_BLOCK_HEADER + TAPEFS_CRC_BYTES)
        return TAPEFS_ERR_INVALID;

    /* Verify CRC */
    uint32_t crc_len = len - TAPEFS_CRC_BYTES;
    uint32_t expected_crc = 0;
    expected_crc |= (uint32_t)buf[crc_len];
    expected_crc |= (uint32_t)buf[crc_len + 1] << 8;
    expected_crc |= (uint32_t)buf[crc_len + 2] << 16;
    expected_crc |= (uint32_t)buf[crc_len + 3] << 24;

    uint32_t actual_crc = tapefs_crc32(buf, crc_len);
    if (actual_crc != expected_crc)
        return TAPEFS_ERR_CRC;

    /* Parse header */
    block->type = buf[0];
    block->seq_no = (uint32_t)buf[1]
                  | ((uint32_t)buf[2] << 8)
                  | ((uint32_t)buf[3] << 16)
                  | ((uint32_t)buf[4] << 24);

    /* Find the layout: data ends where parity begins.
     * Parity is multiples of 16 bytes. CRC is last 4 bytes.
     * We try all possible chunk counts to find matching layout. */

    int found = 0;
    int max_chunks = (TAPEFS_BLOCK_SIZE_MAX / 16) + 1;

    for (int chunks = 0; chunks <= max_chunks; chunks++) {
        uint32_t parity_bytes = (uint32_t)chunks * 16;
        uint32_t payload_end = len - TAPEFS_CRC_BYTES - parity_bytes;

        if (payload_end < TAPEFS_BLOCK_HEADER) break;

        uint32_t data_len  = payload_end - TAPEFS_BLOCK_HEADER;
        if (data_len > TAPEFS_BLOCK_DATA_MAX) continue;

        uint32_t total_bytes = TAPEFS_BLOCK_HEADER + data_len;
        uint32_t expected_chunks = (total_bytes + 238) / 239;
        if (expected_chunks < 1 && data_len > 0) expected_chunks = 1;

        if ((uint32_t)chunks == expected_chunks ||
            (data_len == 0 && chunks == 0)) {
            /* Found the layout */
            block->data_len = data_len;

            uint32_t copy_len = data_len;
            if (copy_len > TAPEFS_BLOCK_DATA_MAX)
                copy_len = TAPEFS_BLOCK_DATA_MAX;
            memset(block->data, 0, TAPEFS_BLOCK_DATA_MAX);
            memcpy(block->data, buf + TAPEFS_BLOCK_HEADER, copy_len);

            /* RS decode each chunk */
            const uint8_t *parity_ptr = buf + payload_end;
            uint32_t offset = 0;

            for (int ci = 0; ci < chunks; ci++) {
                uint32_t chunk_size = total_bytes - offset;
                if (chunk_size > 239) chunk_size = 239;

                uint8_t rs_buf[239];
                memset(rs_buf, 0, 239);
                memcpy(rs_buf, buf + offset, chunk_size);

                tapefs_rs_decode(rs_buf, parity_ptr);

                /* Write corrected data back (payload portion only) */
                for (uint32_t j = 0; j < chunk_size; j++) {
                    uint32_t global_pos = offset + j;
                    if (global_pos >= TAPEFS_BLOCK_HEADER) {
                        uint32_t payload_idx = global_pos - TAPEFS_BLOCK_HEADER;
                        if (payload_idx < block->data_len) {
                            block->data[payload_idx] = rs_buf[j];
                        }
                    }
                }

                parity_ptr += 16;
                offset += chunk_size;
            }

            found = 1;
            break;
        }
    }

    if (!found)
        return TAPEFS_ERR_INVALID;

    return TAPEFS_OK;
}
