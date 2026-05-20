#pragma once
// EpollPoller — epoll(7)-backed I/O event loop for Linux.
//
// Uses EPOLLET (edge-triggered) + EPOLLONESHOT semantics to mirror
// the KqueuePoller behaviour: after each event fires, the fd is
// re-armed via modify() to continue receiving events.
//
// Wakeup via eventfd(2): a single uint64_t write unblocks epoll_wait.

#include "IOPoller.hpp"

#include <atomic>
#include <mutex>
#include <unordered_map>

namespace cortex::serving {

class EpollPoller final : public IOPoller {
public:
    explicit EpollPoller(int max_events = 256);
    ~EpollPoller() override;

    void add(int fd, IOEvent interest, IOCallback cb) override;
    void modify(int fd, IOEvent interest) override;
    void remove(int fd) override;
    void run() override;
    void stop() override;
    void wakeup() override;

private:
    struct FdState {
        IOEvent    interest;
        IOCallback cb;
    };

    uint32_t to_epoll_events(IOEvent interest) const noexcept;
    void     rearm(int fd, IOEvent interest);

    int               epfd_      = -1;
    int               event_fd_  = -1;   // eventfd for wakeup
    int               max_events_;
    std::atomic<bool> stop_flag_{false};

    mutable std::mutex                mu_;
    std::unordered_map<int, FdState>  fds_;
};

} // namespace cortex::serving
