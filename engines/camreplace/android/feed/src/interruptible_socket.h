#pragma once

#include <atomic>
#include <cerrno>
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

} // namespace cr::feed
