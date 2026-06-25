#include "tapefs/firmware/DummyMcu.hpp"
#include <cstdio>

int main() {
    fprintf(stderr, "[DummyMCU-CPP] Firmware simulation ready\n");
    tapefs::firmware::DummyMcu mcu;
    mcu.runStdio();
    return 0;
}
