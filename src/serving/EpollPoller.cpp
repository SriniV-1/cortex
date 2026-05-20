#include "serving/EpollPoller.hpp"
#include "common/Logger.hpp"

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <vector>

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

namespace cortex::serving {

// ── Construction ───────────────────────────────────────────────────────────

EpollPoller::EpollPoller(int max_events)
    : max_events_(max_events)
{
    epfd_ = ::epoll_create1(EPOLL_CLOEXEC);
    if (epfd_ < 0)
        throw std::runtime_error(std::string("epoll_create1: ") + std::strerror(errno));

    // eventfd for wakeup (non-blocking, semaphore mode not needed)
    event_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (event_fd_ < 0) {
        ::close(epfd_);
        throw std::runtime_error(std::string("eventfd: ") + std::strerror(errno));
    }

    // Register eventfd for reading (level-triggered OK for wakeup)
    struct epoll_event ev{};
    ev.events  = EPOLLIN;
    ev.data.fd = event_fd_;
    if (::epoll_ctl(epfd_, EPOLL_CTL_ADD, event_fd_, &ev) < 0) {
        ::close(event_fd_);
        ::close(epfd_);
        throw std::runtime_error(std::string("epoll_ctl wakeup: ") + std::strerror(errno));
    }
}

EpollPoller::~EpollPoller() {
    if (event_fd_ >= 0) { ::close(event_fd_); event_fd_ = -1; }
    if (epfd_ >= 0)     { ::close(epfd_);     epfd_ = -1; }
}

// ── IOPoller interface ─────────────────────────────────────────────────────

uint32_t EpollPoller::to_epoll_events(IOEvent interest) const noexcept {
    uint32_t ev = EPOLLET | EPOLLONESHOT;  // edge-triggered, one-shot
    if (interest & IOEvent::Readable) ev |= EPOLLIN;
    if (interest & IOEvent::Writable) ev |= EPOLLOUT;
    ev |= EPOLLERR | EPOLLHUP | EPOLLRDHUP;
    return ev;
}

void EpollPoller::rearm(int fd, IOEvent interest) {
    struct epoll_event ev{};
    ev.events  = to_epoll_events(interest);
    ev.data.fd = fd;
    ::epoll_ctl(epfd_, EPOLL_CTL_MOD, fd, &ev);
}

void EpollPoller::add(int fd, IOEvent interest, IOCallback cb) {
    {
        std::lock_guard lock(mu_);
        fds_[fd] = FdState{interest, std::move(cb)};
    }

    struct epoll_event ev{};
    ev.events  = to_epoll_events(interest);
    ev.data.fd = fd;
    if (::epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &ev) < 0)
        throw std::runtime_error(std::string("epoll_ctl ADD: ") + std::strerror(errno));
}

void EpollPoller::modify(int fd, IOEvent interest) {
    {
        std::lock_guard lock(mu_);
        auto it = fds_.find(fd);
        if (it == fds_.end()) return;
        it->second.interest = interest;
    }
    rearm(fd, interest);
}

void EpollPoller::remove(int fd) {
    ::epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr);
    std::lock_guard lock(mu_);
    fds_.erase(fd);
}

void EpollPoller::run() {
    auto log = cortex::get_logger("epoll");
    stop_flag_.store(false, std::memory_order_release);

    std::vector<struct epoll_event> events(static_cast<size_t>(max_events_));

    while (!stop_flag_.load(std::memory_order_acquire)) {
        int n = ::epoll_wait(epfd_, events.data(), max_events_, 50 /*ms*/);
        if (n < 0) {
            if (errno == EINTR) continue;
            log->error("epoll_wait: {}", std::strerror(errno));
            break;
        }

        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;

            // Wakeup event: drain the eventfd counter and re-check stop flag.
            if (fd == event_fd_) {
                uint64_t val = 0;
                ::read(event_fd_, &val, sizeof(val));
                continue;
            }

            // Build IOEvent mask from epoll flags.
            IOEvent mask = static_cast<IOEvent>(0);
            uint32_t ef  = events[i].events;
            if (ef & EPOLLIN)                mask = mask | IOEvent::Readable;
            if (ef & EPOLLOUT)               mask = mask | IOEvent::Writable;
            if (ef & EPOLLERR)               mask = mask | IOEvent::Error;
            if (ef & (EPOLLHUP | EPOLLRDHUP))mask = mask | IOEvent::HangUp;

            // Copy callback under lock.
            IOCallback cb;
            IOEvent    interest;
            {
                std::lock_guard lock(mu_);
                auto it = fds_.find(fd);
                if (it == fds_.end()) continue;
                cb       = it->second.cb;
                interest = it->second.interest;
            }

            if (cb) cb(fd, mask);

            // Re-arm after EPOLLONESHOT fires (unless removed by callback).
            {
                std::lock_guard lock(mu_);
                if (fds_.count(fd)) rearm(fd, interest);
            }
        }
    }
}

void EpollPoller::stop() {
    stop_flag_.store(true, std::memory_order_release);
    wakeup();
}

void EpollPoller::wakeup() {
    uint64_t val = 1;
    ::write(event_fd_, &val, sizeof(val));
}

} // namespace cortex::serving
