#include "tapefs.h"
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Filesystem Context                                                 */
/* ------------------------------------------------------------------ */

struct tapefs_fs {
    tapefs_write_fn     write_fn;
    tapefs_read_fn      read_fn;
    tapefs_seek_fn      seek_fn;
    void               *cb_ctx;
    uint32_t            next_block;    /* next block to write/read */
};

tapefs_fs_t *tapefs_create(tapefs_write_fn write_fn,
                           tapefs_read_fn  read_fn,
                           tapefs_seek_fn  seek_fn,
                           void           *cb_ctx) {
    tapefs_fs_t *fs = (tapefs_fs_t *)calloc(1, sizeof(tapefs_fs_t));
    if (!fs) return NULL;

    fs->write_fn  = write_fn;
    fs->read_fn   = read_fn;
    fs->seek_fn   = seek_fn;
    fs->cb_ctx    = cb_ctx;
    fs->next_block = 0;
    return fs;
}

void tapefs_destroy(tapefs_fs_t *fs) {
    if (fs) free(fs);
}

/* ------------------------------------------------------------------ */
/*  Format                                                             */
/* ------------------------------------------------------------------ */

int tapefs_format(tapefs_fs_t *fs) {
    if (!fs->write_fn || !fs->seek_fn)
        return TAPEFS_ERR_INVALID;

    /* Rewind to BOT */
    if (!fs->seek_fn(0, fs->cb_ctx))
        return TAPEFS_ERR_IO;

    /* Write empty directory block at block 0 */
    tapefs_dir_t dir;
    tapefs_dir_init(&dir);
    tapefs_block_t dir_block = tapefs_dir_serialise(&dir);
    tapefs_raw_block_t raw = tapefs_block_serialise(&dir_block);

    /* We might need to wait for seek to complete — for now,
     * this assumes seek_fn is blocking. In a real system,
     * you'd poll GET_STATUS until position is locked. */

    if (!fs->write_fn(&raw, fs->cb_ctx))
        return TAPEFS_ERR_IO;

    /* Write EOT marker at next block */
    tapefs_block_t eot_block;
    memset(&eot_block, 0, sizeof(eot_block));
    eot_block.type   = TAPEFS_BLOCK_EOT;
    eot_block.seq_no = 1;
    eot_block.data_len = 0;

    raw = tapefs_block_serialise(&eot_block);
    if (!fs->write_fn(&raw, fs->cb_ctx))
        return TAPEFS_ERR_IO;

    fs->next_block = 2;
    return TAPEFS_OK;
}

/* ------------------------------------------------------------------ */
/*  Directory Read / Write                                             */
/* ------------------------------------------------------------------ */

int tapefs_read_dir(tapefs_fs_t *fs, tapefs_dir_t *dir) {
    if (!fs->seek_fn || !fs->read_fn)
        return TAPEFS_ERR_INVALID;

    /* Rewind and seek to block 0 (directory) */
    if (!fs->seek_fn(0, fs->cb_ctx))
        return TAPEFS_ERR_IO;

    /* Read directory block */
    tapefs_raw_block_t raw;
    memset(&raw, 0, sizeof(raw));

    if (!fs->read_fn(&raw, fs->cb_ctx))
        return TAPEFS_ERR_IO;

    tapefs_block_t block;
    int ret = tapefs_block_deserialise(&raw, &block);
    if (ret != TAPEFS_OK)
        return ret;

    if (block.type != TAPEFS_BLOCK_DIR)
        return TAPEFS_ERR_INVALID;

    return tapefs_dir_deserialise(&block, dir);
}

int tapefs_write_dir(tapefs_fs_t *fs, const tapefs_dir_t *dir) {
    if (!fs->seek_fn || !fs->write_fn)
        return TAPEFS_ERR_INVALID;

    /* Seek to block 0 (directory) */
    if (!fs->seek_fn(0, fs->cb_ctx))
        return TAPEFS_ERR_IO;

    tapefs_block_t dir_block = tapefs_dir_serialise(dir);
    tapefs_raw_block_t raw = tapefs_block_serialise(&dir_block);

    if (!fs->write_fn(&raw, fs->cb_ctx))
        return TAPEFS_ERR_IO;

    return TAPEFS_OK;
}

/* ------------------------------------------------------------------ */
/*  File Write                                                         */
/* ------------------------------------------------------------------ */

int tapefs_write_file(tapefs_fs_t *fs, const char *filename,
                      const uint8_t *data, uint32_t data_len) {
    if (!fs->write_fn || !fs->read_fn || !fs->seek_fn)
        return TAPEFS_ERR_INVALID;

    /* Read current directory */
    tapefs_dir_t dir;
    int ret = tapefs_read_dir(fs, &dir);
    if (ret != TAPEFS_OK && ret != TAPEFS_ERR_CRC && ret != TAPEFS_ERR_ECC) {
        /* No valid directory exists — start fresh */
        tapefs_dir_init(&dir);
    }

    /* Check if file already exists */
    if (tapefs_dir_find(&dir, filename) != NULL)
        return TAPEFS_ERR_INVALID;

    /* Find next free block (after directory at block 0) */
    uint32_t current_block = 1;

    /* Scan forward to find end of existing data or EOT */
    /* We could read sequentially, but for now assume append at next_block */
    current_block = fs->next_block;
    if (current_block < 1) current_block = 1;

    uint32_t start_block = current_block;
    uint32_t bytes_written = 0;
    uint32_t seq = start_block;

    /* Seek to the first data block position before writing */
    if (!fs->seek_fn(start_block, fs->cb_ctx))
        return TAPEFS_ERR_IO;

    /* Split data into blocks and write */
    while (bytes_written < data_len) {
        tapefs_block_t block;
        memset(&block, 0, sizeof(block));
        block.type   = TAPEFS_BLOCK_DATA;
        block.seq_no = seq;

        uint32_t chunk = data_len - bytes_written;
        if (chunk > TAPEFS_BLOCK_DATA_MAX)
            chunk = TAPEFS_BLOCK_DATA_MAX;

        memcpy(block.data, data + bytes_written, chunk);
        block.data_len = chunk;

        tapefs_raw_block_t raw = tapefs_block_serialise(&block);

        if (!fs->write_fn(&raw, fs->cb_ctx)) {
            /* Write failed — partial write, tape may be corrupt */
            return TAPEFS_ERR_IO;
        }

        bytes_written += chunk;
        seq++;
    }

    /* Write EOT marker */
    tapefs_block_t eot_block;
    memset(&eot_block, 0, sizeof(eot_block));
    eot_block.type   = TAPEFS_BLOCK_EOT;
    eot_block.seq_no = seq;

    tapefs_raw_block_t raw_eot = tapefs_block_serialise(&eot_block);
    if (!fs->write_fn(&raw_eot, fs->cb_ctx))
        return TAPEFS_ERR_IO;

    /* Update directory */
    uint32_t end_block = seq - 1;
    ret = tapefs_dir_add(&dir, filename, data_len,
                          start_block, end_block);
    if (ret != TAPEFS_OK)
        return ret;

    /* Write updated directory back */
    ret = tapefs_write_dir(fs, &dir);
    if (ret != TAPEFS_OK)
        return ret;

    fs->next_block = seq + 1;
    return TAPEFS_OK;
}

/* ------------------------------------------------------------------ */
/*  File Read                                                          */
/* ------------------------------------------------------------------ */

int tapefs_read_file(tapefs_fs_t *fs, const char *filename,
                     uint8_t *buffer, uint32_t *data_len) {
    if (!fs->read_fn || !fs->seek_fn)
        return TAPEFS_ERR_INVALID;

    /* Read directory to find file */
    tapefs_dir_t dir;
    int ret = tapefs_read_dir(fs, &dir);
    if (ret != TAPEFS_OK)
        return ret;

    tapefs_dirent_t *entry = tapefs_dir_find(&dir, filename);
    if (!entry)
        return TAPEFS_ERR_NOT_FOUND;

    /* Seek to start block of file */
    if (!fs->seek_fn(entry->start_block, fs->cb_ctx))
        return TAPEFS_ERR_IO;

    /* Read blocks until we have the full file or hit EOT */
    uint32_t bytes_read = 0;
    uint32_t max_len = *data_len;

    for (uint32_t seq = entry->start_block; seq <= entry->end_block; seq++) {
        tapefs_raw_block_t raw;
        memset(&raw, 0, sizeof(raw));

        if (!fs->read_fn(&raw, fs->cb_ctx)) {
            /* Read error — return what we have */
            break;
        }

        tapefs_block_t block;
        int ret2 = tapefs_block_deserialise(&raw, &block);
        if (ret2 != TAPEFS_OK) {
            /* CRC/ECC error — try next block anyway
             * (we return what we got, caller checks *data_len) */
            continue;
        }

        if (block.type == TAPEFS_BLOCK_EOT)
            break;

        if (block.type != TAPEFS_BLOCK_DATA)
            continue;

        uint32_t copy_len = block.data_len;
        if (bytes_read + copy_len > max_len)
            copy_len = max_len - bytes_read;

        memcpy(buffer + bytes_read, block.data, copy_len);
        bytes_read += copy_len;

        if (bytes_read >= max_len)
            break;
    }

    *data_len = bytes_read;

    /* Check if we got all the data */
    if (bytes_read < entry->file_size)
        return TAPEFS_ERR_CRC; /* partial read — data may be corrupt */

    return TAPEFS_OK;
}

/* ------------------------------------------------------------------ */
/*  List Files / Stat                                                  */
/* ------------------------------------------------------------------ */

int tapefs_list_files(tapefs_fs_t *fs, tapefs_dir_t *dir) {
    return tapefs_read_dir(fs, dir);
}

int tapefs_stat(tapefs_fs_t *fs, uint32_t *total_blocks,
                uint32_t *used_blocks, uint32_t *free_blocks) {
    /* Rough estimate based on tape position.
     * A C60 cassette = ~60 min/side ÷ (block_time).
     * At 200 baud, 1KB blocks: ~10 s/block → ~360 blocks/side.
     *
     * For now, report based on next_block as "used".
     * A more accurate implementation would scan to EOT. */

    if (total_blocks) *total_blocks = 360; /* C60 estimate */
    if (used_blocks)  *used_blocks  = fs->next_block;
    if (free_blocks)  *free_blocks  = 360 - fs->next_block;
    return TAPEFS_OK;
}
