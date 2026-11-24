#pragma once

#include <atomic>
#include <cerrno>
#include <poll.h>
#include <sys/socket.h>

namespace cr::feed
{

// A blocking accept() can survive SIGTERM on Android because signal() may
// install SA_RESTART. Poll in short bounded intervals so the feed process can
// observe its stop token without a client ever connecting.
inline int accept_interruptible(int server,
                                const std::atomic<bool>& stop) noexcept
{
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
      const int client = ::accept(server, nullptr, nullptr);
      if (client >= 0)
        return client;
      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
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
