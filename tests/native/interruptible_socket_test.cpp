#include "interruptible_socket.h"

#include <cassert>
#include <chrono>
#include <netinet/in.h>
#include <thread>

static void verify_transfer_deadlines()
{
  using namespace std::chrono_literals;
  using Socket = cr::feed::DeadlineSocket;
  int pair[2];
  assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
  std::atomic<bool> stop{false};
  const auto started = Socket::Clock::now();
  Socket input(pair[0], stop, 2s, started + 80ms);
  const char sample = 'x';
  assert(::send(pair[1], &sample, 1, 0) == 1);
  char bytes[2]{};
  assert(!input.receive(bytes, sizeof(bytes)) && errno == ETIMEDOUT);
  assert(bytes[0] == sample);
  assert(Socket::Clock::now() - started < 1s);
  input.finish_startup();
  assert(input.send(&sample, 1));
  assert(::recv(pair[1], bytes, 1, 0) == 1 && bytes[0] == sample);

  // A complete chunk may need several reads and an acknowledgement.
  // Finishing one transfer must not extend the deadline for the next.
  const auto chunk_deadline = Socket::Clock::now() + 100ms;
  assert(::send(pair[1], &sample, 1, 0) == 1);
  assert(input.receive(bytes, 1, chunk_deadline) && bytes[0] == sample);
  std::this_thread::sleep_until(chunk_deadline);
  assert(::send(pair[1], &sample, 1, 0) == 1);
  assert(!input.receive(bytes, 1, chunk_deadline) && errno == ETIMEDOUT);
  assert(!input.send(&sample, 1, chunk_deadline) && errno == ETIMEDOUT);
  assert(input.receive(bytes, 1) && bytes[0] == sample);

  stop.store(true, std::memory_order_relaxed);
  assert(!input.receive(bytes, 1) && errno == ECANCELED);
  assert(::close(pair[0]) == 0);
  assert(::close(pair[1]) == 0);
}

int main()
{
  using namespace std::chrono_literals;
  const int server = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  assert(server >= 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  assert(::bind(server, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
  assert(::listen(server, 1) == 0);
  socklen_t length = sizeof(address);
  assert(::getsockname(server, reinterpret_cast<sockaddr*>(&address), &length) == 0);

  const int sender = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  assert(sender >= 0);
  assert(::connect(sender, reinterpret_cast<sockaddr*>(&address), length) == 0);
  std::atomic<bool> stop{false};
  const int accepted = cr::feed::accept_interruptible(server, stop);
  assert(accepted >= 0);
  assert((::fcntl(server, F_GETFL) & O_NONBLOCK) != 0);
  assert((::fcntl(accepted, F_GETFL) & O_NONBLOCK) == 0);
  assert((::fcntl(accepted, F_GETFD) & FD_CLOEXEC) != 0);
  assert(::close(accepted) == 0);
  assert(::close(sender) == 0);

  const auto started = std::chrono::steady_clock::now();
  std::thread request_stop([&stop] {
    std::this_thread::sleep_for(30ms);
    stop.store(true, std::memory_order_relaxed);
  });
  const int cancelled = cr::feed::accept_interruptible(server, stop);
  const int error = errno;
  request_stop.join();
  assert(cancelled == -1 && error == ECANCELED);
  assert(std::chrono::steady_clock::now() - started < 2s);
  assert(::close(server) == 0);
  verify_transfer_deadlines();
}
