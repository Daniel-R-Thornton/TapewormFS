#pragma once
/**
 * Visualiser — real-time terminal display of tape state.
 *
 * Shows:
 *   [TAPE] ============H=========================  block 42
 *   Motor: ▶ PLAY at 47.6 mm/s  (wow ±1.2%)
 *   Modem: IDLE
 *   Packets: 12 sent, 12 OK
 *
 * Usage:
 *   Visualizer viz;
 *   viz.setTapePosition(250.0, 90000.0);
 *   viz.setMotorState("▶ PLAY", 47.6, 1.2);
 *   viz.setModemState("ENCODING");
 *   viz.draw();
 */

#include <cstdio>
#include <string>
#include <cmath>

namespace tapefs { namespace firmware {

class Visualizer {
public:
    void setTapePosition(double mm, double totalMM) {
        mm_ = mm;
        totalMM_ = totalMM;
    }

    void setMotorState(const char* label, double speed, double wowFlutter) {
        motorLabel_ = label;
        motorSpeed_ = speed;
        wowFlutter_ = wowFlutter;
    }

    void setModemState(const char* state) {
        modemState_ = state;
    }

    void setPacketCount(int sent, int ok) {
        packetsSent_ = sent;
        packetsOk_ = ok;
    }

    void setBlockNumber(int block) {
        block_ = block;
    }

    /// Draw one frame.  Call at ~10 Hz.
    void draw() {
        // Move cursor home and clear
        printf("\033[H\033[J");

        // Title
        printf("╔══════════════════════════════════════════╗\n");
        printf("║       TapewormFS — ESP32 Simulator      ║\n");
        printf("╚══════════════════════════════════════════╝\n\n");

        // Tape visualisation
        int width = 50;
        double frac = mm_ / totalMM_;
        if (frac < 0) frac = 0;
        if (frac > 1) frac = 1;
        int headPos = static_cast<int>(frac * width);

        printf("  TAPE: ");
        for (int i = 0; i < width; i++) {
            if (i == headPos) printf("\033[7mH\033[0m");  // highlighted head
            else printf("═");
        }
        printf("\n        BOT%*s%.0f mm%*sEOT\n\n",
               headPos - 3, "", mm_, width - headPos - 5, "");

        // Block info
        printf("  Block: %d\n\n", block_);

        // Motor
        printf("  Motor: %s  at %.1f mm/s", motorLabel_.c_str(), motorSpeed_);
        if (wowFlutter_ > 0.01)
            printf("  (wow/flutter ±%.1f%%)", wowFlutter_);
        printf("\n\n");

        // Modem
        printf("  Modem: %s\n\n", modemState_.c_str());

        // Packets
        int errs = packetsSent_ - packetsOk_;
        printf("  Packets: %d sent, %d OK", packetsSent_, packetsOk_);
        if (errs > 0) printf(", %d errors", errs);
        printf("\n\n");

        // Key
        printf("  ───  Controls ───\n");
        printf("   P: PING     F: Format    W: Write file\n");
        printf("   R: Read     S: Seek      Q: Quit\n");

        fflush(stdout);
    }

private:
    double mm_ = 0;
    double totalMM_ = 90000.0;
    std::string motorLabel_ = "STOPPED";
    double motorSpeed_ = 0;
    double wowFlutter_ = 0;
    std::string modemState_ = "IDLE";
    int packetsSent_ = 0;
    int packetsOk_ = 0;
    int block_ = 0;
};

}} // namespace
