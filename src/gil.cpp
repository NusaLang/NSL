#include "gil.hpp"

GIL& GIL::instance() {
    // Heap-allocated, never deleted (same as GC::instance()) -- a
    // detached goroutine can still be locking/unlocking this at exit,
    // and a never-destroyed singleton can't race its own destructor.
    static GIL* gil = new GIL();
    return *gil;
}

void GIL::lock() { mu_.lock(); }
void GIL::unlock() { mu_.unlock(); }
