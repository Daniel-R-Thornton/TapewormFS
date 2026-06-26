#!/bin/bash
# tapeworm — mount the tape drive with the dummy MCU.
#
# Keeps the firmware simulation running in the background so
# multiple commands can access the same tape state.
#
# Usage:
#   ./tapeworm mount <mountpoint>    mount as FUSE folder
#   ./tapeworm ls                    list files
#   ./tapeworm format                format tape
#   ./tapeworm read <name> [out]     read file
#   ./tapeworm write <local> [name]  write file
#   ./tapeworm sync <folder>         sync folder
#   ./tapeworm stop                  stop firmware

set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
FIRMWARE="$DIR/run_firmware"
HOST_DRIVER="$DIR/../../host_driver.py"

# Build firmware if needed
if [ ! -f "$FIRMWARE" ]; then
    echo "Building firmware simulation..."
    cd "$DIR"
    g++ -std=c++17 -o run_firmware \
        -x c++ - <<< '#include "firmware.hpp"
        int main() { tapefs::firmware::Firmware fw; fw.run(); return 0; }' \
        firmware.cpp modem_encoder.cpp modem_decoder.cpp \
        tape_medium.cpp tape_deck.cpp motor.cpp esp32_hal.cpp \
        -lpthread -I.
    cd - > /dev/null
fi

# FIFO for persistent firmware connection
FIFO="/tmp/tapeworm_fifo"
FIRMWARE_PID_FILE="/tmp/tapeworm_fw.pid"

start_firmware() {
    if [ -f "$FIRMWARE_PID_FILE" ] && kill -0 $(cat "$FIRMWARE_PID_FILE") 2>/dev/null; then
        return 0  # already running
    fi
    rm -f "$FIFO"
    mkfifo "$FIFO"
    # Start firmware connected to FIFO
    cat "$FIFO" | "$FIRMWARE" | cat > "$FIFO" &
    FW_PID=$!
    echo "$FW_PID" > "$FIRMWARE_PID_FILE"
    # Give it a moment to start
    sleep 0.3
}

stop_firmware() {
    if [ -f "$FIRMWARE_PID_FILE" ]; then
        kill $(cat "$FIRMWARE_PID_FILE") 2>/dev/null || true
        rm -f "$FIRMWARE_PID_FILE"
    fi
    rm -f "$FIFO"
}

# Trap exit to clean up
trap stop_firmware EXIT

case "${1:-help}" in
    mount)
        MOUNTPOINT="${2:-./tape_mount}"
        start_firmware
        echo "Mounting at $MOUNTPOINT with firmware simulation..."
        cat "$FIFO" | python3 "$HOST_DRIVER" --port stdio mount "$MOUNTPOINT"
        stop_firmware
        ;;
    ls|format)
        start_firmware
        cat "$FIFO" | python3 "$HOST_DRIVER" --port stdio "$@"
        ;;
    read|write|sync)
        start_firmware
        cat "$FIFO" | python3 "$HOST_DRIVER" --port stdio "$@"
        ;;
    stop)
        stop_firmware
        echo "Firmware stopped"
        ;;
    help)
        echo "Usage: $0 mount <mountpoint>"
        echo "       $0 ls"
        echo "       $0 format"
        echo "       $0 read <filename> [output]"
        echo "       $0 write <localfile> [tapenam]"
        echo "       $0 sync <folder>"
        echo "       $0 stop"
        ;;
    *)
        "$0" help
        exit 1
        ;;
esac
