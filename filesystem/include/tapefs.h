#ifndef TAPEFS_H
#define TAPEFS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/*  Constants                                                          */
/* ------------------------------------------------------------------ */

#define TAPEFS_MAGIC            "TWF"
#define TAPEFS_BLOCK_DATA_MAX   1000    /* max payload bytes per block */
#define TAPEFS_ECC_PARITY_BYTES 16      /* RS(255,239) parity bytes    */
#define TAPEFS_BLOCK_HEADER     5       /* type(1) + seq_no(4)         */
#define TAPEFS_CRC_BYTES        4       /* CRC-32 trailing bytes       */
#define TAPEFS_BLOCK_SIZE_MAX   (TAPEFS_BLOCK_HEADER +     \
                                 TAPEFS_BLOCK_DATA_MAX +   \
                                 TAPEFS_ECC_PARITY_BYTES + \
                                 TAPEFS_CRC_BYTES)          /* 1025 */
#define TAPEFS_MAX_FILES        32      /* max files in directory      */
#define TAPEFS_FILENAME_LEN     20      /* bytes including null term   */
#define TAPEFS_DIR_ENTRY_SIZE   32      /* bytes per directory entry   */
#define TAPEFS_MAX_RETRIES      3       /* read retry count            */

/* ------------------------------------------------------------------ */
/*  Block Type Enums                                                   */
/* ------------------------------------------------------------------ */

typedef enum {
    TAPEFS_BLOCK_DATA      = 0x01,
    TAPEFS_BLOCK_DIR       = 0x02,
    TAPEFS_BLOCK_FAT       = 0x03,
    TAPEFS_BLOCK_ECC       = 0x04,
    TAPEFS_BLOCK_EOT       = 0xFF,
} tapefs_block_type_t;

/* ------------------------------------------------------------------ */
/*  Directory Entry                                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    char     filename[TAPEFS_FILENAME_LEN];  /* null-padded ASCII     */
    uint32_t start_block;                     /* first block number    */
    uint32_t end_block;                       /* last block number     */
    uint32_t file_size;                       /* total file size bytes */
} tapefs_dirent_t;

/* ------------------------------------------------------------------ */
/*  Directory                                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    tapefs_dirent_t files[TAPEFS_MAX_FILES];
    uint32_t        file_count;
} tapefs_dir_t;

/* ------------------------------------------------------------------ */
/*  Raw Block (wire format, serialised)                                */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t             type;           /* tapefs_block_type_t         */
    uint32_t            seq_no;         /* sequential block number     */
    uint8_t             data[TAPEFS_BLOCK_DATA_MAX];
    uint32_t            data_len;       /* bytes actually used in data */
    /* ECC parity and CRC appended during serialise, not stored here */
} tapefs_block_t;

/* ------------------------------------------------------------------ */
/*  Serialised Block Buffer (full wire representation)                 */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t bytes[TAPEFS_BLOCK_SIZE_MAX];
    uint32_t len;                        /* total serialised length    */
} tapefs_raw_block_t;

/* ------------------------------------------------------------------ */
/*  Filesystem Context                                                 */
/* ------------------------------------------------------------------ */

typedef struct tapefs_fs tapefs_fs_t;

/* Callbacks: the filesystem layer needs a transport to talk to the
 * MCU. These are provided by the host-driver or firmware.
 *
 * write_block_fn:  send one raw block to the tape (returns true on ACK)
 * read_block_fn:   read one raw block from tape (returns true on success)
 * seek_fn:         seek to block number (non-blocking, poll for completion)
 * rewind_fn:       rewind to BOT (non-blocking)
 * get_position_fn: returns current TapePosition (see transport protocol)
 */

typedef bool (*tapefs_write_fn)(const tapefs_raw_block_t *block, void *ctx);
typedef bool (*tapefs_read_fn)(tapefs_raw_block_t *block, void *ctx);
typedef bool (*tapefs_seek_fn)(uint32_t block_no, void *ctx);

/* ------------------------------------------------------------------ */
/*  Error Codes                                                        */
/* ------------------------------------------------------------------ */

typedef enum {
    TAPEFS_OK               = 0,
    TAPEFS_ERR_IO           = -1,
    TAPEFS_ERR_CRC          = -2,
    TAPEFS_ERR_ECC          = -3,
    TAPEFS_ERR_NOT_FOUND    = -4,
    TAPEFS_ERR_FULL         = -5,
    TAPEFS_ERR_INVALID      = -6,
    TAPEFS_ERR_NO_TAPE      = -7,
    TAPEFS_ERR_TIMEOUT      = -8,
} tapefs_error_t;

/* ------------------------------------------------------------------ */
/*  Reed-Solomon ECC                                                   */
/* ------------------------------------------------------------------ */

/* RS(255, 239) — corrects up to 8 byte errors per block.
 *   data:     239 bytes (or fewer, zero-padded)
 *   parity:   16 bytes output
 *   errata:   positions and values of errors to correct
 */

void tapefs_rs_encode(const uint8_t data[239], uint8_t parity[16]);
int  tapefs_rs_decode(uint8_t data[239], const uint8_t parity[16]);

/* ------------------------------------------------------------------ */
/*  CRC-32                                                             */
/* ------------------------------------------------------------------ */

uint32_t tapefs_crc32(const uint8_t *buf, uint32_t len);

/* ------------------------------------------------------------------ */
/*  Block Serialisation                                                */
/* ------------------------------------------------------------------ */

/* Serialise a logical block into wire format (adds ECC + CRC).
 * Returns the raw block ready to send over the transport. */
tapefs_raw_block_t tapefs_block_serialise(const tapefs_block_t *block);

/* Deserialise a wire-format block back into a logical block.
 * Verifies CRC and attempts ECC correction.
 * Returns TAPEFS_OK on success, or error code. */
int tapefs_block_deserialise(const tapefs_raw_block_t *raw,
                             tapefs_block_t *block);

/* ------------------------------------------------------------------ */
/*  Directory Operations                                               */
/* ------------------------------------------------------------------ */

/* Initialise an empty directory. */
void tapefs_dir_init(tapefs_dir_t *dir);

/* Add a file entry to a directory. Returns TAPEFS_OK or error. */
int tapefs_dir_add(tapefs_dir_t *dir, const char *filename,
                   uint32_t file_size, uint32_t start_block,
                   uint32_t end_block);

/* Find a file by name. Returns pointer to entry or NULL. */
tapefs_dirent_t *tapefs_dir_find(tapefs_dir_t *dir, const char *filename);

/* Remove a file entry by name. Returns TAPEFS_OK or error. */
int tapefs_dir_remove(tapefs_dir_t *dir, const char *filename);

/* Serialise a directory into a data block (for writing to tape). */
tapefs_block_t tapefs_dir_serialise(const tapefs_dir_t *dir);

/* Deserialise a data block into a directory. */
int tapefs_dir_deserialise(const tapefs_block_t *block, tapefs_dir_t *dir);

/* ------------------------------------------------------------------ */
/*  High-Level Filesystem API                                          */
/* ------------------------------------------------------------------ */

/* Create a filesystem context bound to transport callbacks. */
tapefs_fs_t *tapefs_create(tapefs_write_fn write_fn,
                           tapefs_read_fn  read_fn,
                           tapefs_seek_fn  seek_fn,
                           void           *cb_ctx);

/* Destroy a filesystem context. */
void tapefs_destroy(tapefs_fs_t *fs);

/* Format the tape: write an empty directory block at block 0,
 * then an EOT marker. This ERASES the tape layout. */
int tapefs_format(tapefs_fs_t *fs);

/* Read the directory from tape (rewinds, reads block 0). */
int tapefs_read_dir(tapefs_fs_t *fs, tapefs_dir_t *dir);

/* Write a directory back to tape (overwrites block 0). */
int tapefs_write_dir(tapefs_fs_t *fs, const tapefs_dir_t *dir);

/* Write a file to tape. Splits data into blocks, writes sequentially.
 * Updates the directory entry on completion. */
int tapefs_write_file(tapefs_fs_t *fs, const char *filename,
                      const uint8_t *data, uint32_t data_len);

/* Read a file from tape. Assembles blocks from tape into buffer.
 * Returns TAPEFS_ERR_NOT_FOUND if file not in directory. */
int tapefs_read_file(tapefs_fs_t *fs, const char *filename,
                     uint8_t *buffer, uint32_t *data_len);

/* List all files on tape. Wrapper around tapefs_read_dir. */
int tapefs_list_files(tapefs_fs_t *fs, tapefs_dir_t *dir);

/* Get tape metadata (total blocks, used blocks, free blocks). */
int tapefs_stat(tapefs_fs_t *fs, uint32_t *total_blocks,
                uint32_t *used_blocks, uint32_t *free_blocks);

#ifdef __cplusplus
}
#endif

#endif /* TAPEFS_H */
