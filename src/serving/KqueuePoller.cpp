#include "serving/KqueuePoller.hpp"
#include "common/Logger.hpp"

#include <stdexcept>
#include <cerrno>
#include <cstring>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>

namespace cortex::serving {

static void set_nonblock(int fd) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        throw std::runtime_error(std::string("fcntl nonblock: ") + std::strerror(errno));
}

KqueuePoller::KqueuePoller(int max_events)
    : max_events_(max_events)
{
    kq_ = ::kqueue();
    if (kq_ < 0)
        throw std::runtime_error(std::string("kqueue(): ") + std::strerror(errno));

    int pipefd[2];
    if (::pipe(pipefd) < 0)
        throw std::runtime_error(std::string("pipe(): ") + std::strerror(errno));

    wake_rd_ = pipefd[0];
    wake_wr_ = pipefd[1];
    set_nonblock(wake_rd_);
    set_nonblock(wake_wr_);

    // Register the read end of the wakeup pipe as readable
    struct kevent ev{};
    EV_SET(&ev, wake_rd_, EVFILT_READ, EV_ADD | EV_ENABLE | EV_CLEAR, 0, 0, nullptr);
    if (::kevent(kq_, &ev, 1, nullptr, 0, nullptr) < 0)
        throw std::runtime_error(std::string("kevent add wakeup pipe: ") + std::strerror(errno));
}

KqueuePoller::~KqueuePoller() {
    if (wake_rd_ >= 0) ::close(wake_rd_);
    if (wake_wr_ >= 0) ::close(wake_wr_);
    if (kq_     >= 0) ::close(kq_);
}

void KqueuePoller::apply_kevent(int fd, IOEvent interest, uint16_t action) {
    // We register separate kevents for READ and WRITE filters
    struct kevent evs[2];
    int n = 0;

    if (action == EV_DELETE) {
        EV_SET(&evs[n++], fd, EVFILT_READ,  EV_DELETE, 0, 0, nullptr);
        EV_SET(&evs[n++], fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
    } else {
        uint16_t read_flags  = (interest & IOEvent::Readable)
                             ? (action | EV_ENABLE | EV_CLEAR)
                             : (EV_ADD | EV_DISABLE);
        uint16_t write_flags = (interest & IOEvent::Writable)
                             ? (action | EV_ENABLE | EV_CLEAR)
                             : (EV_ADD | EV_DISABLE);
        EV_SET(&evs[n++], fd, EVFILT_READ,  read_flags,  0, 0, nullptr);
        EV_SET(&evs[n++], fd, EVFILT_WRITE, write_flags, 0, 0, nullptr);
    }

    // Ignore ENOENT on delete (fd may not have been registered for that filter)
    for (int i = 0; i < n; ++i) {
        if (::kevent(kq_, &evs[i], 1, nullptr, 0, nullptr) < 0
            && errno != ENOENT && errno != EBADF) {
            auto log = cortex::get_logger("kqueue");
            log->warn("kevent fd={} filter={} action={}: {}",
                      fd, evs[i].filter, action, std::strerror(errno));
        }
    }
}

void KqueuePoller::add(int fd, IOEvent interest, IOCallback cb) {
    {
        std::lock_guard lock(mu_);
        fds_[fd] = {interest, std::move(cb)};
    }
    apply_kevent(fd, interest, EV_ADD);
}

void KqueuePoller::modify(int fd, IOEvent interest) {
    {
        std::lock_guard lock(mu_);
        auto it = fds_.find(fd);
        if (it == fds_.end()) return;
        it->second.interest = interest;
    }
    apply_kevent(fd, interest, EV_ADD);  // EV_ADD on existing fd acts as modify
}

void KqueuePoller::remove(int fd) {
    {
        std::lock_guard lock(mu_);
        fds_.erase(fd);
    }
    apply_kevent(fd, IOEvent::Readable, EV_DELETE);
}

void KqueuePoller::run() {
    auto log = cortex::get_logger("kqueue");
    log->info("KqueuePoller started");

    std::vector<struct kevent> events(max_events_);

    while (!stop_flag_.load(std::memory_order_acquire)) {
        struct timespec timeout{0, 50'000'000};  // 50ms max wait
        int n = ::kevent(kq_, nullptr, 0, events.data(),
                         static_cast<int>(events.size()), &timeout);

        if (n < 0) {
            if (errno == EINTR) continue;
            log->error("kevent wait: {}", std::strerror(errno));
            break;
        }

        for (int i = 0; i < n; ++i) {
            const auto& kev = events[i];
            int fd = static_cast<int>(kev.ident);

            // Drain wakeup pipe
            if (fd == wake_rd_) {
                char buf[64];
                while (::read(wake_rd_, buf, sizeof(buf)) > 0) {}
                continue;
            }

            // Build event mask
            IOEvent ev_mask{};
            if (kev.filter == EVFILT_READ)  ev_mask = ev_mask | IOEvent::Readable;
            if (kev.filter == EVFILT_WRITE) ev_mask = ev_mask | IOEvent::Writable;
            if (kev.flags  & EV_EOF)        ev_mask = ev_mask | IOEvent::HangUp;
            if (kev.flags  & EV_ERROR)      ev_mask = ev_mask | IOEvent::Error;

            // Invoke callback (under shared lock to guard map access)
            IOCallback cb;
            {
                std::lock_guard lock(mu_);
                auto it = fds_.find(fd);
                if (it == fds_.end()) continue;
                cb = it->second.cb;
            }
            if (cb) cb(fd, ev_mask);
        }
    }

    log->info("KqueuePoller stopped");
}

void KqueuePoller::stop() {
    stop_flag_.store(true, std::memory_order_release);
    wakeup();
}

void KqueuePoller::wakeup() {
    char b = 1;
    (void)::write(wake_wr_, &b, 1);
}

} // namespace cortex::serving
