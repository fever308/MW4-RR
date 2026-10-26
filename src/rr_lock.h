#pragma once
#include <mutex>
namespace rrlock {
// A recursive mutex that counts its owner's holds, so a call into another add-on can give up
// every hold this thread has and take them back afterwards (Released).
class Recursive {
    std::recursive_mutex m;
    unsigned depth{}; // changed only by the owner, with m held

  public:
    void lock() {
        m.lock();
        ++depth;
    }
    bool try_lock() {
        if (!m.try_lock())
            return false;
        ++depth;
        return true;
    }
    void unlock() {
        --depth;
        m.unlock();
    }
    // The owner's holds, all released; 0 when this thread holds none it can see.
    unsigned ReleaseAll() {
        const unsigned n = depth;
        for (unsigned i = 0; i < n; ++i)
            unlock();
        return n;
    }
    void Reacquire(unsigned n) {
        for (unsigned i = 0; i < n; ++i)
            lock();
    }
};
// Runs f with every hold of the calling thread on lock released, and takes them back after f
// returns or throws. Only the owner may call it.
template <class F> auto Released(Recursive &lock, F &&f) -> decltype(f()) {
    struct Hold {
        Recursive &lock;
        unsigned n;
        ~Hold() {
            lock.Reacquire(n);
        }
    } hold{lock, lock.ReleaseAll()};
    return f();
}
} // namespace rrlock
