#include "tapefs/firmware/TapeDeck.hpp"
#include "tapefs/firmware/Motor.hpp"
#include "tapefs/firmware/TapeHead.hpp"
#include "tapefs/firmware/Modem.hpp"
#include <cstdio>
#include <string>
#include <thread>
#include <chrono>

using namespace tapefs::firmware;

static int passed = 0, failed = 0;

#define TEST(name) printf("  %-50s ... ", name); fflush(stdout)
#define PASS() do { printf("OK\n"); passed++; } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); failed++; } while(0)

void testMotor() {
    TEST("Motor accelerates to play speed");
    Motor m;
    m.play();

    // Tick at 100Hz for 0.5 seconds (simulated)
    for (int i = 0; i < 50; i++) m.tick(0.01);

    double speed = m.currentSpeedMMps();
    if (speed > 40.0 && speed < 55.0) { PASS(); }
    else { FAIL(("speed=" + std::to_string(speed)).c_str()); }
}

void testMotorStops() {
    TEST("Motor decelerates to stop");
    Motor m;
    m.play();
    for (int i = 0; i < 50; i++) m.tick(0.01);
    m.stop();
    for (int i = 0; i < 50; i++) m.tick(0.01);
    if (!m.isMoving()) { PASS(); }
    else { FAIL("still moving"); }
}

void testMotorWowFlutter() {
    TEST("Motor wow/flutter varies speed");
    Motor m;
    m.play();

    double speeds[100];
    for (int i = 0; i < 100; i++) {
        m.tick(0.01);
        speeds[i] = m.effectiveSpeedMMps();
    }

    // Check that speed varies (wow/flutter is working)
    double minS = speeds[0], maxS = speeds[0];
    for (auto s : speeds) {
        if (s < minS) minS = s;
        if (s > maxS) maxS = s;
    }
    double variation = (maxS - minS) / 47.6 * 100;
    if (variation > 0.5) { PASS(); }
    else { FAIL(("variation=" + std::to_string(variation) + "%").c_str()); }
}

void testTapeDeckFormatWriteRead() {
    TEST("TapeDeck format → write → read");
    auto deck = TapeDeck();

    // Simulate writing a few blocks
    deck.writeBuffer({0x01, 0x00, 0x00, 0x00, 'H', 'i'});
    deck.writeBuffer({0x01, 0x01, 0x00, 0x00, 'H', 'e', 'l', 'l', 'o'});
    deck.flush();

    if (deck.blockCount() != 2) {
        FAIL(("blocks=" + std::to_string(deck.blockCount())).c_str());
        return;
    }

    // Read back
    auto block = deck.readNextBlock();
    if (!block) { FAIL("no block"); return; }
    if (block->size() != 6) { FAIL("wrong size"); return; }

    PASS();
}

void testTapeDeckSeek() {
    TEST("TapeDeck seek changes position");
    auto deck = TapeDeck();

    // Write blocks at different positions
    for (int i = 0; i < 10; i++)
        deck.writeBuffer({static_cast<uint8_t>(i)});
    deck.flush();

    int before = deck.currentBlock();

    // Seek to block 5
    deck.seekToBlock(5);
    // Tick until arrived
    for (int i = 0; i < 200; i++) {
        deck.tick();
        if (!deck.isBusy()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    if (deck.currentBlock() >= 4 && deck.currentBlock() <= 6) { PASS(); }
    else { FAIL(("block=" + std::to_string(deck.currentBlock())).c_str()); }
}

void testModemEncoder() {
    TEST("ModemEncoder produces audio samples");
    auto enc = ModemEncoder();
    enc.encode({0x41, 0x42});  // "AB"

    size_t count = enc.output().size();
    if (count > 0) { PASS(); }
    else { FAIL("no output"); }
}

int main() {
    printf("TapewormFS Firmware Tests\n");
    printf("========================\n\n");

    testMotor();
    testMotorStops();
    testMotorWowFlutter();
    testTapeDeckFormatWriteRead();
    testTapeDeckSeek();
    testModemEncoder();

    printf("\nResults: %d passed, %d failed\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
