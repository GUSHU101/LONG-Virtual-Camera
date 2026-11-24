#include "device/AdbClient.h"
#include "util/Log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <cstring>
#include <iterator>
#include <limits>
#include <sstream>
namespace fs = std::filesystem;

namespace cr::device
{

// Win32 process helpers (internal)

namespace
{

constexpr size_t kMaxCapturedBytes = 8u * 1024u * 1024u;
constexpr size_t kMaxStreamingLineBytes = 64u * 1024u;

class InheritedHandleList
{
public:
  InheritedHandleList() = default;
  ~InheritedHandleList()
  {
    if (list_)
    {
      if (initialized_)
        DeleteProcThreadAttributeList(list_);
      HeapFree(GetProcessHeap(), 0, list_);
    }
  }
  InheritedHandleList(const InheritedHandleList&) = delete;
  InheritedHandleList& operator=(const InheritedHandleList&) = delete;

  bool initialize(HANDLE* handles, size_t count) noexcept
  {
    if (!handles || count == 0 || count > std::numeric_limits<DWORD>::max())
      return false;
    SIZE_T bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    if (bytes == 0)
      return false;
    list_ = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
        HeapAlloc(GetProcessHeap(), 0, bytes));
    if (!list_ || !InitializeProcThreadAttributeList(list_, 1, 0, &bytes))
      return false;
    initialized_ = true;
    return UpdateProcThreadAttribute(list_, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                     handles, count * sizeof(HANDLE), nullptr, nullptr) != FALSE;
  }

  [[nodiscard]] LPPROC_THREAD_ATTRIBUTE_LIST get() const noexcept
  {
    return initialized_ ? list_ : nullptr;
  }

private:
  LPPROC_THREAD_ATTRIBUTE_LIST list_ = nullptr;
  bool initialized_ = false;
};

// Build a Windows command line from argv[] according to CommandLineToArgvW
// quoting rules. Each argument wrapped in double quotes, embedded quotes
// escaped with backslash, trailing backslashes doubled.
std::wstring quote_arg(const std::string& s)
{
  std::wstring w;
  // Empty argument still needs "".
  bool need_quote = s.empty() || s.find_first_of(" \t\n\v\"") != std::string::npos;

  auto mb = [](const std::string& in)
  {
    int n = MultiByteToWideChar(CP_UTF8, 0, in.data(), (int) in.size(), nullptr, 0);
    std::wstring out(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, in.data(), (int) in.size(), out.data(), n);
    return out;
  };

  std::wstring src = mb(s);
  if (!need_quote)
    return src;

  w.push_back(L'"');
  for (size_t i = 0; i < src.size(); ++i)
  {
    size_t bs = 0;
    while (i < src.size() && src[i] == L'\\')
    {
      ++bs;
      ++i;
    }
    if (i == src.size())
    {
      w.append(bs * 2, L'\\');
      break;
    }
    if (src[i] == L'"')
    {
      w.append(bs * 2 + 1, L'\\');
    }
    else
    {
      w.append(bs, L'\\');
    }
    w.push_back(src[i]);
  }
  w.push_back(L'"');
  return w;
}

std::wstring build_cmdline(const fs::path& exe, const std::vector<std::string>& args)
{
  std::wstring cmd;
  cmd.push_back(L'"');
  cmd += exe.wstring();
  cmd.push_back(L'"');
  for (auto& a : args)
  {
    cmd.push_back(L' ');
    cmd.append(quote_arg(a));
  }
  return cmd;
}

void append_bounded(std::string& sink, const char* data, size_t bytes,
                    bool& truncated)
{
  if (!data || bytes == 0)
    return;
  const size_t used = std::min(sink.size(), kMaxCapturedBytes);
  const size_t copy = std::min(bytes, kMaxCapturedBytes - used);
  if (copy != 0)
    sink.append(data, copy);
  if (copy != bytes)
    truncated = true;
}

void drain_available(HANDLE h, std::string& sink, bool& truncated)
{
  if (!h)
    return;

  for (;;)
  {
    DWORD avail = 0;
    if (!PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr) || avail == 0)
    {
      return;
    }

    char buf[4096];
    const DWORD want = avail < sizeof(buf) ? avail : static_cast<DWORD>(sizeof(buf));
    DWORD n = 0;
    if (!ReadFile(h, buf, want, &n, nullptr) || n == 0)
    {
      return;
    }
    append_bounded(sink, buf, n, truncated);
  }
}

void append_truncation_notice(std::string& sink, bool truncated)
{
  if (truncated)
    sink.append("\n[output truncated at 8 MiB]\n");
}

void notify_exit_noexcept(const AdbClient::ExitCallback& callback, int code) noexcept
{
  if (!callback)
    return;
  try
  {
    callback(code);
  }
  catch (const std::exception& e)
  {
    cr::log::warn("adb", std::string("exit callback failed: ") + e.what());
  }
  catch (...)
  {
    cr::log::warn("adb", "exit callback failed");
  }
}

void notify_line_noexcept(const AdbClient::LineCallback& callback,
                          std::string line) noexcept
{
  if (!callback)
    return;
  try
  {
    callback(std::move(line));
  }
  catch (const std::exception& e)
  {
    cr::log::warn("adb", std::string("line callback failed: ") + e.what());
  }
  catch (...)
  {
    cr::log::warn("adb", "line callback failed");
  }
}

// One-shot process spawner that captures stdout/stderr. Blocks.
// timeout_ms > 0 terminates the child after that many milliseconds.
AdbResult spawn_capture(const fs::path& exe, const std::vector<std::string>& args, int timeout_ms = 30000)
{
  AdbResult res;

  // Pipes for child stdout/stderr. Use separate pipes so we can distinguish.
  SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
  HANDLE out_r = nullptr, out_w = nullptr;
  HANDLE err_r = nullptr, err_w = nullptr;

  if (!CreatePipe(&out_r, &out_w, &sa, 0) || !CreatePipe(&err_r, &err_w, &sa, 0))
  {
    res.stderr_ = "CreatePipe failed";
    if (out_r)
      CloseHandle(out_r);
    if (out_w)
      CloseHandle(out_w);
    if (err_r)
      CloseHandle(err_r);
    if (err_w)
      CloseHandle(err_w);
    return res;
  }
  if (!SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0) ||
      !SetHandleInformation(err_r, HANDLE_FLAG_INHERIT, 0))
  {
    res.stderr_ = "SetHandleInformation failed";
    CloseHandle(out_r);
    CloseHandle(out_w);
    CloseHandle(err_r);
    CloseHandle(err_w);
    return res;
  }

  HANDLE null_in = CreateFileW(L"NUL", GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (null_in == INVALID_HANDLE_VALUE)
  {
    res.stderr_ = "cannot open NUL for adb stdin";
    CloseHandle(out_r);
    CloseHandle(out_w);
    CloseHandle(err_r);
    CloseHandle(err_w);
    return res;
  }

  STARTUPINFOEXW si{};
  si.StartupInfo.cb = sizeof(si);
  si.StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
  si.StartupInfo.wShowWindow = SW_HIDE;
  si.StartupInfo.hStdOutput = out_w;
  si.StartupInfo.hStdError = err_w;
  si.StartupInfo.hStdInput = null_in;
  HANDLE inherited[] = {out_w, err_w, null_in};
  InheritedHandleList handle_list;
  if (!handle_list.initialize(inherited, std::size(inherited)))
  {
    res.stderr_ = "cannot restrict adb handle inheritance";
    CloseHandle(null_in);
    CloseHandle(out_r);
    CloseHandle(out_w);
    CloseHandle(err_r);
    CloseHandle(err_w);
    return res;
  }
  si.lpAttributeList = handle_list.get();

  PROCESS_INFORMATION pi{};
  std::wstring cmdline = build_cmdline(exe, args);

  BOOL ok = CreateProcessW(exe.c_str(),
                           cmdline.data(), // mutable buffer required
                           nullptr, nullptr, TRUE,
                           CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
                           nullptr, nullptr, &si.StartupInfo, &pi);

  CloseHandle(null_in);
  CloseHandle(out_w);
  CloseHandle(err_w);

  if (!ok)
  {
    DWORD err = GetLastError();
    res.stderr_ = "CreateProcess failed err=" + std::to_string(err);
    CloseHandle(out_r);
    CloseHandle(err_r);
    return res;
  }

  // Pump both pipes from this thread. This avoids creating helper reader
  // threads, which ScyllaHide's "Prevent Thread creation" hook can turn into
  // std::system_error -> std::terminate inside background workers.
  bool stdout_truncated = false;
  bool stderr_truncated = false;
  const ULONGLONG t0 = GetTickCount64();
  for (;;)
  {
    const DWORD wait = WaitForSingleObject(pi.hProcess, 15);
    drain_available(out_r, res.stdout_, stdout_truncated);
    drain_available(err_r, res.stderr_, stderr_truncated);
    if (wait == WAIT_OBJECT_0)
      break;
    if (wait == WAIT_FAILED)
    {
      const DWORD wait_error = GetLastError();
      TerminateProcess(pi.hProcess, 1);
      WaitForSingleObject(pi.hProcess, 1000);
      res.stderr_ += (res.stderr_.empty() ? "" : "; ");
      res.stderr_ += "WaitForSingleObject failed err=" + std::to_string(wait_error);
      break;
    }
    if (timeout_ms > 0 && (GetTickCount64() - t0) >= static_cast<ULONGLONG>(timeout_ms))
    {
      TerminateProcess(pi.hProcess, 1);
      res.stderr_ += (res.stderr_.empty() ? "" : "; ");
      res.stderr_ += "adb command timed out after " + std::to_string(timeout_ms) + "ms";
      // Drain once more after kill, then fall through to exit code.
      WaitForSingleObject(pi.hProcess, 1000);
      drain_available(out_r, res.stdout_, stdout_truncated);
      drain_available(err_r, res.stderr_, stderr_truncated);
      break;
    }
  }
  drain_available(out_r, res.stdout_, stdout_truncated);
  drain_available(err_r, res.stderr_, stderr_truncated);
  append_truncation_notice(res.stdout_, stdout_truncated);
  append_truncation_notice(res.stderr_, stderr_truncated);

  DWORD code = 0;
  if (GetExitCodeProcess(pi.hProcess, &code))
    res.exit_code = (int) code;
  else
    res.stderr_ += (res.stderr_.empty() ? "" : "; ") +
                   std::string("GetExitCodeProcess failed err=") +
                   std::to_string(GetLastError());

  CloseHandle(out_r);
  CloseHandle(err_r);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return res;
}

} // namespace

// AdbStream — streaming command handle

struct AdbStream::Impl
{
  HANDLE process = nullptr;
  HANDLE out_pipe = nullptr;
  std::thread reader;
  std::atomic<bool> running{false};
  AdbClient::ExitCallback on_exit;
};

AdbStream::AdbStream() = default;
AdbStream::AdbStream(AdbStream&&) noexcept = default;
AdbStream& AdbStream::operator=(AdbStream&&) noexcept = default;

AdbStream::~AdbStream()
{
  stop();
}

void AdbStream::stop()
{
  if (!impl_)
    return;
  impl_->running = false;
  if (impl_->process)
  {
    TerminateProcess(impl_->process, 0);
    WaitForSingleObject(impl_->process, 500);
  }
  if (impl_->reader.joinable())
  {
    CancelSynchronousIo(impl_->reader.native_handle());
    impl_->reader.join();
  }
  if (impl_->out_pipe)
    CloseHandle(impl_->out_pipe);
  if (impl_->process)
    CloseHandle(impl_->process);
  impl_.reset();
}

bool AdbStream::is_running() const noexcept
{
  return impl_ && impl_->running.load();
}

// AdbClient

AdbClient& AdbClient::instance()
{
  static AdbClient c;
  return c;
}

AdbClient::AdbClient()
{
  try
  {
    worker_ = std::thread(
        [this]
        {
          try
          {
            worker_loop();
          }
          catch (...)
          {
            running_ = false;
          }
        });
  }
  catch (...)
  {
    running_ = false;
  }
}

AdbClient::~AdbClient()
{
  {
    std::lock_guard<std::mutex> lk(mu_);
    running_ = false;
    cv_.notify_all();
  }
  if (worker_.joinable())
    worker_.join();
}

void AdbClient::set_adb_path(fs::path p)
{
  const std::string display = p.string();
  {
    std::lock_guard<std::mutex> lock(path_mu_);
    adb_path_ = std::move(p);
  }
  cr::log::info("adb", "adb path: " + display);
}

void AdbClient::clear_adb_path()
{
  std::lock_guard<std::mutex> lock(path_mu_);
  adb_path_.clear();
}

fs::path AdbClient::copy_adb_path_() const
{
  std::lock_guard<std::mutex> lock(path_mu_);
  return adb_path_;
}

fs::path AdbClient::adb_path() const
{
  return copy_adb_path_();
}

void AdbClient::worker_loop()
{
  for (;;)
  {
    Task task;
    {
      std::unique_lock<std::mutex> lk(mu_);
      cv_.wait(lk, [&] { return !queue_.empty() || !running_; });
      if (!running_ && queue_.empty())
        return;
      task = std::move(queue_.front());
      queue_.pop_front();
    }
    const fs::path path = copy_adb_path_();
    AdbResult r = path.empty() ? AdbResult{-1, "", "adb path not set"}
                               : spawn_capture(path, task.args);
    if (task.cb)
    {
      try
      {
        task.cb(std::move(r));
      }
      catch (const std::exception& e)
      {
        cr::log::warn("adb", std::string("completion callback failed: ") + e.what());
      }
      catch (...)
      {
        cr::log::warn("adb", "completion callback failed");
      }
    }
  }
}

void AdbClient::submit(Task t)
{
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (running_.load(std::memory_order_relaxed))
    {
      queue_.push_back(std::move(t));
      cv_.notify_one();
      return;
    }
  }
  if (t.cb)
  {
    try
    {
      t.cb(AdbResult{-1, "", "adb worker unavailable"});
    }
    catch (...)
    {
      cr::log::warn("adb", "unavailable callback failed");
    }
  }
}

AdbResult AdbClient::run_sync(const std::vector<std::string>& args, int timeout_ms)
{
  const fs::path path = copy_adb_path_();
  if (path.empty())
    return {-1, "", "adb path not set"};
  return spawn_capture(path, args, timeout_ms);
}

AdbResult AdbClient::run_sync_to(const std::string& serial,
                                const std::vector<std::string>& args,
                                int timeout_ms)
{
  std::vector<std::string> a{"-s", serial};
  a.insert(a.end(), args.begin(), args.end());
  return run_sync(a, timeout_ms);
}

void AdbClient::run(const std::vector<std::string>& args, Callback cb)
{
  submit({args, std::move(cb)});
}

void AdbClient::run_to(const std::string& serial, const std::vector<std::string>& args, Callback cb)
{
  std::vector<std::string> a{"-s", serial};
  a.insert(a.end(), args.begin(), args.end());
  run(a, std::move(cb));
}

void AdbClient::start_server()
{
  run({"start-server"}, nullptr);
}

void AdbClient::kill_server()
{
  run({"kill-server"}, nullptr);
}

AdbResult AdbClient::kill_server_sync(int timeout_ms)
{
  return run_sync({"kill-server"}, timeout_ms);
}

std::vector<std::string> AdbClient::su_shell(std::string cmd)
{
  // Escape any single quote inside the body using the classic '\'' trick
  // so the outer quoting stays intact.
  std::string escaped;
  escaped.reserve(cmd.size() + 8);
  for (char c : cmd)
  {
    if (c == '\'')
      escaped += "'\\''";
    else
      escaped += c;
  }
  // Two-argument form: adb gets exactly `shell` and one blob. No matter
  // how it joins trailing args internally, there are no extra args to mis-
  // tokenise, so the entire `su -c '...'` reaches the device verbatim.
  return {"shell", "su -c '" + escaped + "'"};
}

// ---- streaming -------------------------------------------------------------

AdbStream AdbClient::run_streaming(const std::vector<std::string>& args, LineCallback on_line, ExitCallback on_exit)
{
  AdbStream stream;
  stream.impl_ = std::make_unique<AdbStream::Impl>();
  auto& im = *stream.impl_;

  const fs::path adb_path = copy_adb_path_();
  if (adb_path.empty())
  {
    notify_exit_noexcept(on_exit, -1);
    return stream;
  }

  SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
  HANDLE out_r = nullptr, out_w = nullptr;
  if (!CreatePipe(&out_r, &out_w, &sa, 64 * 1024))
  {
    notify_exit_noexcept(on_exit, -1);
    return stream;
  }
  if (!SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0))
  {
    CloseHandle(out_r);
    CloseHandle(out_w);
    notify_exit_noexcept(on_exit, -1);
    return stream;
  }

  HANDLE null_in = CreateFileW(L"NUL", GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (null_in == INVALID_HANDLE_VALUE)
  {
    CloseHandle(out_r);
    CloseHandle(out_w);
    notify_exit_noexcept(on_exit, -1);
    return stream;
  }

  STARTUPINFOEXW si{};
  si.StartupInfo.cb = sizeof(si);
  si.StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
  si.StartupInfo.wShowWindow = SW_HIDE;
  si.StartupInfo.hStdOutput = out_w;
  si.StartupInfo.hStdError = out_w; // merge stderr for simplicity of line reader
  si.StartupInfo.hStdInput = null_in;
  HANDLE inherited[] = {out_w, null_in};
  InheritedHandleList handle_list;
  if (!handle_list.initialize(inherited, std::size(inherited)))
  {
    CloseHandle(null_in);
    CloseHandle(out_r);
    CloseHandle(out_w);
    notify_exit_noexcept(on_exit, -1);
    return stream;
  }
  si.lpAttributeList = handle_list.get();

  PROCESS_INFORMATION pi{};
  std::wstring cmdline = build_cmdline(adb_path, args);

  BOOL ok = CreateProcessW(adb_path.c_str(), cmdline.data(), nullptr, nullptr,
                           TRUE, CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
                           nullptr, nullptr, &si.StartupInfo, &pi);

  CloseHandle(null_in);
  CloseHandle(out_w);
  if (!ok)
  {
    CloseHandle(out_r);
    notify_exit_noexcept(on_exit, -1);
    return stream;
  }

  CloseHandle(pi.hThread);
  im.process = pi.hProcess;
  im.out_pipe = out_r;
  im.on_exit = std::move(on_exit);
  im.running = true;

  try
  {
    im.reader = std::thread(
        [impl = stream.impl_.get(), on_line]() mutable
        {
          std::string buf;
          char tmp[4096];
          DWORD n = 0;
          while (ReadFile(impl->out_pipe, tmp, sizeof(tmp), &n, nullptr) && n > 0)
          {
            buf.append(tmp, n);
            // Flush complete lines.
            for (;;)
            {
              auto nl = buf.find('\n');
              if (nl == std::string::npos)
                break;
              std::string line = buf.substr(0, nl);
              if (!line.empty() && line.back() == '\r')
                line.pop_back();
              notify_line_noexcept(on_line, std::move(line));
              buf.erase(0, nl + 1);
            }
            if (buf.size() > kMaxStreamingLineBytes)
            {
              buf.erase(0, buf.size() - kMaxStreamingLineBytes);
              buf.insert(0, "[line prefix truncated] ");
            }
          }
          if (!buf.empty())
            notify_line_noexcept(on_line, std::move(buf));

          // Child exited or pipe closed.
          DWORD code = 0;
          GetExitCodeProcess(impl->process, &code);
          impl->running = false;
          notify_exit_noexcept(impl->on_exit, (int) code);
        });
  }
  catch (...)
  {
    im.running = false;
    TerminateProcess(im.process, 0);
    CloseHandle(im.out_pipe);
    CloseHandle(im.process);
    im.out_pipe = nullptr;
    im.process = nullptr;
    notify_exit_noexcept(im.on_exit, -1);
  }

  return stream;
}

AdbStream AdbClient::run_streaming_to(const std::string& serial, const std::vector<std::string>& args, LineCallback on_line, ExitCallback on_exit)
{
  std::vector<std::string> a{"-s", serial};
  a.insert(a.end(), args.begin(), args.end());
  return run_streaming(a, std::move(on_line), std::move(on_exit));
}

} // namespace cr::device
