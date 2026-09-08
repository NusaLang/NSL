#pragma once

#include <mutex>

// Process-wide Global Interpreter Lock -- at most one thread ever executes
// Nusantara code at a time (same model as Python's GIL). Released around
// blocking I/O so other goroutines can run meanwhile; no CPU parallelism.
class GIL {
public:
    static GIL& instance();
    void lock();
    void unlock();

private:
    std::mutex mu_;
};

// RAII: releases the GIL for the duration of a blocking call, reacquires
// it on scope exit.
struct GilRelease {
    GilRelease() { GIL::instance().unlock(); }
    ~GilRelease() { GIL::instance().lock(); }
    GilRelease(const GilRelease&) = delete;
    GilRelease& operator=(const GilRelease&) = delete;
};
