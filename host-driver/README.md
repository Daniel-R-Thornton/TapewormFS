# Host Driver

Local application that presents the tape drive as a sequential
filesystem to the OS (FUSE / 9P / etc).

Responsibilities:
- Buffer data from /tmp (disk-backed streaming)
- Handle extreme latency (minutes-long reads/writes)
- Present as sequential block device to OS
- Multiplex between web debugger and local access
