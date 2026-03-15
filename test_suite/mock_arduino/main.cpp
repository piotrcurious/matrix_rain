#include "Arduino.h"
SerialMock Serial;
int main() {
    std::srand(std::time(nullptr));
    setup();
    while (true) { loop(); }
    return 0;
}
