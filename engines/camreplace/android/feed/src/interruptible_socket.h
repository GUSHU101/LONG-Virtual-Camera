#pragma once

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace cr::feed
{

// Readiness is advisory: the pending connection may be gone by accept().
// Keep the listening descriptor nonblocking, while accepted sockets retain
// blocking I/O so existing connection timeouts still apply.
inline int accept_interruptible(int server,
                                const std::atomic<bool>& stop) noexcept
{
  const int flags = ::fcntl(server, F_GETFL);
  if (flags == -1 ||
      ((flags & O_NONBLOCK) == 0 && ::fcntl(server, F_SETFL, flags | O_NONBLOCK) == -1))
    return -1;
  while (!stop.load(std::memory_order_relaxed))
  {
    pollfd descriptor{};
    descriptor.fd = server;
    descriptor.events = POLLIN;
    const int ready = ::poll(&descriptor, 1, 250);
    if (ready == 0)
      continue;
    if (ready < 0)
    {
      if (errno == EINTR)
        continue;
      return -1;
    }
    if ((descriptor.revents & POLLIN) != 0)
    {
      if (stop.load(std::memory_order_relaxed))
        break;
      const int client = ::accept4(server, nullptr, nullptr, SOCK_CLOEXEC);
      if (client >= 0)
      {
        if (stop.load(std::memory_order_relaxed))
        {
          ::close(client);
          break;
        }
        return client;
      }
      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK || errno == ECONNABORTED)
        continue;
      return -1;
    }
    if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
    {
      errno = ECONNABORTED;
      return -1;
    }
  }
  errno = ECANCELED;
  return -1;
}

// Each transfer has a fixed deadline. A shared session deadline can cap a
// sequence of transfers, including partial reads and writes during startup.
class DeadlineSocket
{
public:
  using Clock = std::chrono::steady_clock;

  DeadlineSocket(int fd, const std::atomic<bool>& stop, Clock::duration io_timeout,
                 Clock::time_point startup_deadline) noexcept
      : fd_(fd), stop_(stop), io_timeout_(io_timeout), deadline_(startup_deadline)
  {
  }

  bool receive(void* data, std::size_t length) const noexcept
  {
    return transfer(static_cast<unsigned char*>(data), length, POLLIN,
        [this](auto* ptr, std::size_t count) {
          return ::recv(fd_, ptr, count, MSG_DONTWAIT);
        });
  }

  bool send(const void* data, std::size_t length) const noexcept
  {
    return transfer(static_cast<const unsigned char*>(data), length, POLLOUT,
        [this](auto* ptr, std::size_t count) {
          return ::send(fd_, ptr, count, MSG_DONTWAIT | MSG_NOSIGNAL);
        });
  }

  void finish_startup() noexcept { deadline_ = Clock::time_point::max(); }

private:
  template <class Byte, class Operation>
  bool transfer(Byte* data, std::size_t length, short event, Operation operation) const noexcept
  {
    const auto per_io = Clock::now() + io_timeout_;
    const auto until = deadline_ < per_io ? deadline_ : per_io;
    while (length != 0)
    {
      if (stop_.load(std::memory_order_relaxed))
      {
        errno = ECANCELED;
        return false;
      }
      const auto remaining = until - Clock::now();
      if (remaining <= Clock::duration::zero())
      {
        errno = ETIMEDOUT;
        return false;
      }
      const auto ms = std::chrono::ceil<std::chrono::milliseconds>(remaining).count();
      pollfd descriptor{fd_, event, 0};
      const int ready = ::poll(&descriptor, 1, ms > 250 ? 250 : static_cast<int>(ms));
      if (ready == 0 || (ready < 0 && errno == EINTR))
        continue;
      if (ready < 0)
        return false;
      if ((descriptor.revents & event) == 0)
      {
        errno = ECONNRESET;
        return false;
      }
      // Poll is advisory. Recheck the shared bound before every syscall.
      if (Clock::now() >= until || stop_.load(std::memory_order_relaxed))
        continue;
      constexpr std::size_t max_syscall = 1u << 20;
      const ssize_t transferred = operation(data, length > max_syscall ? max_syscall : length);
      if (transferred == 0)
      {
        errno = ECONNRESET;
        return false;
      }
      if (transferred < 0)
      {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
          continue;
        return false;
      }
      data += transferred;
      length -= static_cast<std::size_t>(transferred);
    }
    return true;
  }

  int fd_;
  const std::atomic<bool>& stop_;
  Clock::duration io_timeout_;
  Clock::time_point deadline_;
};

} // namespace cr::feed
