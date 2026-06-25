#include "tapefs.h"
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Directory Operations                                               */
/* ------------------------------------------------------------------ */

void tapefs_dir_init(tapefs_dir_t *dir) {
    dir->file_count = 0;
    memset(dir->files, 0, sizeof(dir->files));
}

int tapefs_dir_add(tapefs_dir_t *dir, const char *filename,
                   uint32_t file_size, uint32_t start_block,
                   uint32_t end_block) {
    if (dir->file_count >= TAPEFS_MAX_FILES)
        return TAPEFS_ERR_FULL;

    /* Check for duplicate */
    if (tapefs_dir_find(dir, filename) != NULL)
        return TAPEFS_ERR_INVALID;

    tapefs_dirent_t *entry = &dir->files[dir->file_count];
    memset(entry, 0, sizeof(tapefs_dirent_t));

    size_t name_len = strlen(filename);
    if (name_len >= TAPEFS_FILENAME_LEN)
        name_len = TAPEFS_FILENAME_LEN - 1;
    memcpy(entry->filename, filename, name_len);
    entry->filename[name_len] = '\0';

    entry->file_size   = file_size;
    entry->start_block = start_block;
    entry->end_block   = end_block;

    dir->file_count++;
    return TAPEFS_OK;
}

tapefs_dirent_t *tapefs_dir_find(tapefs_dir_t *dir, const char *filename) {
    for (uint32_t i = 0; i < dir->file_count; i++) {
        if (strncmp(dir->files[i].filename, filename,
                    TAPEFS_FILENAME_LEN) == 0) {
            return &dir->files[i];
        }
    }
    return NULL;
}

int tapefs_dir_remove(tapefs_dir_t *dir, const char *filename) {
    for (uint32_t i = 0; i < dir->file_count; i++) {
        if (strncmp(dir->files[i].filename, filename,
                    TAPEFS_FILENAME_LEN) == 0) {
            /* Shift remaining entries down */
            uint32_t remaining = dir->file_count - i - 1;
            if (remaining > 0) {
                memmove(&dir->files[i], &dir->files[i + 1],
                        remaining * sizeof(tapefs_dirent_t));
            }
            dir->file_count--;
            memset(&dir->files[dir->file_count], 0,
                   sizeof(tapefs_dirent_t));
            return TAPEFS_OK;
        }
    }
    return TAPEFS_ERR_NOT_FOUND;
}

/* ------------------------------------------------------------------ */
/*  Directory Serialisation                                            */
/* ------------------------------------------------------------------ */
/*
 * Wire format within a data block:
 *   [0..2]   magic "TWF"
 *   [3]      file_count
 *   [4..]    dirent[0], dirent[1], ...
 *
 * Each dirent is TAPEFS_DIR_ENTRY_SIZE (32) bytes:
 *   [0..19]  filename (null-padded)
 *   [20..23] start_block (LE)
 *   [24..27] end_block (LE)
 *   [28..31] file_size (LE)
 */

tapefs_block_t tapefs_dir_serialise(const tapefs_dir_t *dir) {
    tapefs_block_t block;
    memset(&block, 0, sizeof(block));

    block.type    = TAPEFS_BLOCK_DIR;
    block.seq_no  = 0; /* directory is always block 0 */

    uint8_t *p = block.data;

    /* Magic */
    p[0] = 'T';
    p[1] = 'W';
    p[2] = 'F';

    /* File count */
    p[3] = (uint8_t)(dir->file_count & 0xFF);

    uint32_t pos = 4;

    for (uint32_t i = 0; i < dir->file_count; i++) {
        const tapefs_dirent_t *entry = &dir->files[i];

        /* Filename (null-padded to 20 bytes) */
        memset(p + pos, 0, TAPEFS_FILENAME_LEN);
        size_t name_len = strlen(entry->filename);
        if (name_len > TAPEFS_FILENAME_LEN)
            name_len = TAPEFS_FILENAME_LEN;
        memcpy(p + pos, entry->filename, name_len);
        pos += TAPEFS_FILENAME_LEN;

        /* start_block (LE) */
        p[pos + 0] = (uint8_t)(entry->start_block & 0xFF);
        p[pos + 1] = (uint8_t)((entry->start_block >> 8) & 0xFF);
        p[pos + 2] = (uint8_t)((entry->start_block >> 16) & 0xFF);
        p[pos + 3] = (uint8_t)((entry->start_block >> 24) & 0xFF);
        pos += 4;

        /* end_block (LE) */
        p[pos + 0] = (uint8_t)(entry->end_block & 0xFF);
        p[pos + 1] = (uint8_t)((entry->end_block >> 8) & 0xFF);
        p[pos + 2] = (uint8_t)((entry->end_block >> 16) & 0xFF);
        p[pos + 3] = (uint8_t)((entry->end_block >> 24) & 0xFF);
        pos += 4;

        /* file_size (LE) */
        p[pos + 0] = (uint8_t)(entry->file_size & 0xFF);
        p[pos + 1] = (uint8_t)((entry->file_size >> 8) & 0xFF);
        p[pos + 2] = (uint8_t)((entry->file_size >> 16) & 0xFF);
        p[pos + 3] = (uint8_t)((entry->file_size >> 24) & 0xFF);
        pos += 4;
    }

    block.data_len = pos;
    return block;
}

int tapefs_dir_deserialise(const tapefs_block_t *block, tapefs_dir_t *dir) {
    if (block->type != TAPEFS_BLOCK_DIR)
        return TAPEFS_ERR_INVALID;

    if (block->data_len < 4)
        return TAPEFS_ERR_INVALID;

    const uint8_t *p = block->data;

    /* Check magic */
    if (p[0] != 'T' || p[1] != 'W' || p[2] != 'F')
        return TAPEFS_ERR_INVALID;

    tapefs_dir_init(dir);
    dir->file_count = p[3];

    if (dir->file_count > TAPEFS_MAX_FILES)
        dir->file_count = TAPEFS_MAX_FILES;

    uint32_t pos = 4;

    for (uint32_t i = 0; i < dir->file_count; i++) {
        if (pos + TAPEFS_DIR_ENTRY_SIZE > block->data_len)
            break;

        tapefs_dirent_t *entry = &dir->files[i];

        /* Filename */
        memcpy(entry->filename, p + pos, TAPEFS_FILENAME_LEN);
        entry->filename[TAPEFS_FILENAME_LEN - 1] = '\0'; /* safety null */
        pos += TAPEFS_FILENAME_LEN;

        /* start_block */
        entry->start_block = (uint32_t)p[pos + 0]
                           | ((uint32_t)p[pos + 1] << 8)
                           | ((uint32_t)p[pos + 2] << 16)
                           | ((uint32_t)p[pos + 3] << 24);
        pos += 4;

        /* end_block */
        entry->end_block = (uint32_t)p[pos + 0]
                         | ((uint32_t)p[pos + 1] << 8)
                         | ((uint32_t)p[pos + 2] << 16)
                         | ((uint32_t)p[pos + 3] << 24);
        pos += 4;

        /* file_size */
        entry->file_size = (uint32_t)p[pos + 0]
                         | ((uint32_t)p[pos + 1] << 8)
                         | ((uint32_t)p[pos + 2] << 16)
                         | ((uint32_t)p[pos + 3] << 24);
        pos += 4;
    }

    return TAPEFS_OK;
}
