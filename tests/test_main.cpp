#include <iostream>

#include "testing.hpp"

int main() {
    int failed = 0;
    for (auto& tc : ddrtest::registry()) {
        try {
            tc.fn();
            std::cout << "[PASS] " << tc.name << "\n";
        } catch (const std::exception& e) {
            std::cout << "[FAIL] " << tc.name << ": " << e.what() << "\n";
            ++failed;
        }
    }
    std::cout << (ddrtest::registry().size() - static_cast<size_t>(failed)) << " passed, "
              << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
