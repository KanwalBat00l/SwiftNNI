#include <iostream>
#include <cassert>
#include "ResourceManager.hpp"

void debug_resource_manager() {
    std::cout << "[DEBUG] Testing ResourceManager..." << std::endl;

    // 1. Initialize with 10 cores and 3 ports (range 100-102)
    ResourceManager rm(10, 100, 3);

    // 2. Test Core Acquisition
    assert(rm.tryAcquireCores(4) == true);
    assert(rm.getFreeCores() == 6);
    assert(rm.tryAcquireCores(7) == false); // Not enough cores
    rm.releaseCores(2);
    assert(rm.getFreeCores() == 8);

    // 3. Test Port Management
    int p1 = rm.acquirePort();
    int p2 = rm.acquirePort();
    int p3 = rm.acquirePort();
    int p4 = rm.acquirePort();

    assert(p1 == 100);
    assert(p4 == -1); // Pool exhausted

    rm.releasePort(p1);
    int p5 = rm.acquirePort();
    assert(p5 == 100); // Reused port

    std::cout << "[DEBUG] All Resource Tests Passed!" << std::endl;
}

int main() {
    debug_resource_manager();
    return 0;
}