/**
 * Visualiser demo — shows the tape state in real time.
 *
 * Usage:
 *   ./viz_demo
 *
 * Shows a simulated tape seek from block 0 to block 50.
 */

#include "visualizer.hpp"
#include "tape_deck.hpp"
#include "motor.hpp"
#include <chrono>
#include <thread>

using namespace tapefs::firmware;

int main() {
    Visualizer viz;
    Motor motor;
    motor.play();

    printf("\033[?25l"); // hide cursor
    fflush(stdout);

    // Simulate seeking to block 50
    double targetMM = 300.0 + 50 * 12.0; // leader + 50 blocks × 12mm
    double startMM = 0;
    double durationMs = (targetMM - startMM) / 476.0 * 1000.0;
    auto startTime = std::chrono::steady_clock::now();

    while (true) {
        auto now = std::chrono::steady_clock::now();
        double elapsedMs = std::chrono::duration<double, std::milli>(now - startTime).count();
        double frac = std::min(1.0, elapsedMs / durationMs);
        double pos = startMM + (targetMM - startMM) * frac;

        // Update visualizer
        viz.setTapePosition(pos, 90000.0);
        viz.setBlockNumber(static_cast<int>(pos / 12.0));
        viz.setPacketCount(42, 40);

        if (frac < 0.5) {
            viz.setMotorState("▶▶ FAST", 476.0, 1.5);
            viz.setModemState("SEEKING");
        } else if (frac < 0.7) {
            viz.setMotorState("▶ PLAY", 47.6, 0.8);
            viz.setModemState("SYNCING");
        } else if (frac < 0.9) {
            viz.setMotorState("▶ PLAY", 47.6, 1.2);
            viz.setModemState("READING block 50");
        } else {
            viz.setMotorState("■ STOPPED", 0, 0);
            viz.setModemState("IDLE — READY");
        }

        viz.draw();

        if (frac >= 1.0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    printf("\033[?25h\033[H\033[J"); // show cursor, clear
    printf("Seek complete. Block 50 ready.\n");
    return 0;
}
