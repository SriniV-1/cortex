#pragma once
// KqueuePoller — kqueue(2)-backed I/O event loop for macOS/BSD.
//
// Internally uses a pipe for wakeup (write 1 byte → poller wakes, re-evaluates).
// Edge-triggered via EV_CLEAR.

#include "IOPoller.hpp"

#include <atomic>
#include <unordered_map>
#include <mutex>
#include <sys/event.h>

namespace cortex::serving {

class KqueuePoller final : public IOPoller {
public:
    explicit KqueuePoller(int max_events = 256);
    ~KqueuePoller() override;

    void add(int fd, IOEvent interest, IOCallback cb) override;
    void modify(int fd, IOEvent interest) override;
    void remove(int fd) override;
    void run() override;
    void stop() override;
    void wakeup() override;

private:
    struct FdState {
        IOEvent   interest;
        IOCallback cb;
    };

    void apply_kevent(int fd, IOEvent interest, uint16_t action);

    int              kq_         = -1;
    int              wake_rd_    = -1;  // pipe read end — registered in kqueue
    int              wake_wr_    = -1;  // pipe write end — used by wakeup()/stop()
    int              max_events_;
    std::atomic<bool> stop_flag_{false};

    mutable std::mutex           mu_;
    std::unordered_map<int, FdState> fds_;
};

} // namespace cortex::serving
