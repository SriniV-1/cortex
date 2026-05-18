#pragma once
// IOPoller — platform-agnostic I/O event loop interface.
//
// Implementations:
//   KqueuePoller  (macOS / BSD)  — src/serving/KqueuePoller.cpp
//   EpollPoller   (Linux)        — src/serving/EpollPoller.cpp  (future)
//
// Design: edge-triggered, non-blocking. Callers register fds with an
// interest mask and receive a callback on readable / writable / error.
// The run() call blocks until stop() is called from another thread.

#include <cstdint>
#include <functional>
#include <sys/types.h>

namespace cortex::serving {

// Events that can be reported for a registered fd.
enum class IOEvent : uint32_t {
    Readable  = 0x01,
    Writable  = 0x02,
    Error     = 0x04,
    HangUp    = 0x08,
};

inline IOEvent operator|(IOEvent a, IOEvent b) {
    return static_cast<IOEvent>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
inline bool operator&(IOEvent a, IOEvent b) {
    return (static_cast<uint32_t>(a) & static_cast<uint32_t>(b)) != 0;
}

// Callback invoked on the poller thread whenever an event fires.
using IOCallback = std::function<void(int fd, IOEvent events)>;

class IOPoller {
public:
    virtual ~IOPoller() = default;

    // Register fd for the given event mask. Replaces any prior registration.
    virtual void add(int fd, IOEvent interest, IOCallback cb) = 0;

    // Update interest mask for an already-registered fd.
    virtual void modify(int fd, IOEvent interest) = 0;

    // Remove fd from the poller (does not close the fd).
    virtual void remove(int fd) = 0;

    // Run the event loop until stop() is called. Blocks the calling thread.
    virtual void run() = 0;

    // Signal the event loop to exit. Thread-safe; may be called from any thread.
    virtual void stop() = 0;

    // Wake the event loop without changing any fd interest (used to re-evaluate
    // pending writes etc. from producer threads). Non-blocking.
    virtual void wakeup() = 0;
};

} // namespace cortex::serving
