# Tape Filesystem

Block allocation, file tables, and directory structure for
storing data on audio cassette.

Design constraints:
- Sequential access (no seeking)
- Extreme latency (tens of seconds to minutes)
- Error-prone media (audio noise, dropouts)
- Limited block size (fits in modem frame)
