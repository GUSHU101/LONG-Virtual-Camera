#include "source/DesktopSource.h"
#include "util/Log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dshow.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cwchar>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <string_view>
#include <utility>
#include <vector>

namespace cr::source
{
namespace
{

using Microsoft::WRL::ComPtr;

class ComScope
{
public:
  ComScope() noexcept : result_(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
  ~ComScope() noexcept
  {
    if (result_ == S_OK || result_ == S_FALSE)
      CoUninitialize();
  }
  [[nodiscard]] bool usable() const noexcept
  {
    return SUCCEEDED(result_) || result_ == RPC_E_CHANGED_MODE;
  }

private:
  HRESULT result_;
};

std::wstring utf8_to_wide(const std::string& text)
{
  if (text.empty())
    return {};
  const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                        text.data(), static_cast<int>(text.size()),
                                        nullptr, 0);
  if (count <= 0)
    return {};
  std::wstring out(static_cast<std::size_t>(count), L'\0');
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                          static_cast<int>(text.size()), out.data(), count) != count)
    return {};
  return out;
}

std::string wide_to_utf8(const wchar_t* text)
{
  if (!text || !*text)
    return {};
  const int chars = static_cast<int>(wcslen(text));
  const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                        text, chars, nullptr, 0, nullptr, nullptr);
  if (count <= 0)
    return {};
  std::string out(static_cast<std::size_t>(count), '\0');
  if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, chars,
                          out.data(), count, nullptr, nullptr) != count)
    return {};
  return out;
}

std::wstring quote_arg(const std::wstring& arg)
{
  if (arg.empty())
    return L"\"\"";
  if (arg.find_first_of(L" \t\n\v\"") == std::wstring::npos)
    return arg;
  std::wstring out = L"\"";
  std::size_t slashes = 0;
  for (wchar_t ch : arg)
  {
    if (ch == L'\\')
    {
      ++slashes;
      continue;
    }
    if (ch == L'\"')
    {
      out.append(slashes * 2 + 1, L'\\');
      out.push_back(L'\"');
    }
    else
    {
      out.append(slashes, L'\\');
      out.push_back(ch);
    }
    slashes = 0;
  }
  out.append(slashes * 2, L'\\');
  out.push_back(L'\"');
  return out;
}

std::wstring join_command(const std::wstring& exe,
                          const std::vector<std::wstring>& args)
{
  std::wstring command = quote_arg(exe);
  for (const auto& arg : args)
  {
    command.push_back(L' ');
    command.append(quote_arg(arg));
  }
  return command;
}

std::wstring find_ffmpeg()
{
  auto existing_absolute = [](const std::wstring& candidate) -> std::wstring
  {
    if (candidate.empty())
      return {};
    std::array<wchar_t, 32768> absolute{};
    const DWORD size = GetFullPathNameW(candidate.c_str(),
                                        static_cast<DWORD>(absolute.size()),
                                        absolute.data(), nullptr);
    if (size == 0 || size >= absolute.size())
      return {};
    const DWORD attrs = GetFileAttributesW(absolute.data());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY)
               ? std::wstring(absolute.data(), size)
               : std::wstring{};
  };

  std::array<wchar_t, 32768> configured{};
  const DWORD configured_size = GetEnvironmentVariableW(
      L"LONGVC_FFMPEG", configured.data(), static_cast<DWORD>(configured.size()));
  if (configured_size > 0 && configured_size < configured.size())
  {
    if (auto path = existing_absolute(configured.data()); !path.empty())
      return path;
  }

  // Prefer an explicitly bundled/adjacent binary. Do not use SearchPath's
  // default order because it includes the process current directory, which
  // lets an unrelated ffmpeg.exe shadow the operator's intended install.
  std::array<wchar_t, 32768> module{};
  const DWORD module_size = GetModuleFileNameW(
      nullptr, module.data(), static_cast<DWORD>(module.size()));
  if (module_size > 0 && module_size < module.size())
  {
    const auto adjacent = (std::filesystem::path(
        std::wstring(module.data(), module_size)).parent_path() / L"ffmpeg.exe").wstring();
    if (auto path = existing_absolute(adjacent); !path.empty())
      return path;
  }

  std::array<wchar_t, 32768> path_env{};
  const DWORD path_size = GetEnvironmentVariableW(
      L"PATH", path_env.data(), static_cast<DWORD>(path_env.size()));
  if (path_size == 0 || path_size >= path_env.size())
    return {};

  // Rebuild PATH without empty/relative components; both mean "current
  // directory" to Windows process search and are inappropriate for loading
  // an executable that will ingest user-selected media.
  std::wstring safe_path;
  std::wstring_view all(path_env.data(), path_size);
  size_t start = 0;
  while (start <= all.size())
  {
    const size_t delimiter = all.find(L';', start);
    std::wstring part(all.substr(start, delimiter == std::wstring_view::npos
                                           ? all.size() - start
                                           : delimiter - start));
    if (part.size() >= 2 && part.front() == L'"' && part.back() == L'"')
      part = part.substr(1, part.size() - 2);
    const bool drive_absolute = part.size() >= 3 &&
                                ((part[0] >= L'A' && part[0] <= L'Z') ||
                                 (part[0] >= L'a' && part[0] <= L'z')) &&
                                part[1] == L':' &&
                                (part[2] == L'\\' || part[2] == L'/');
    const bool unc_absolute = part.size() >= 2 && part[0] == L'\\' && part[1] == L'\\';
    if (drive_absolute || unc_absolute)
    {
      if (!safe_path.empty())
        safe_path.push_back(L';');
      safe_path.append(part);
    }
    if (delimiter == std::wstring_view::npos)
      break;
    start = delimiter + 1;
  }
  if (safe_path.empty())
    return {};

  std::array<wchar_t, 32768> found{};
  const DWORD size = SearchPathW(safe_path.c_str(), L"ffmpeg.exe", nullptr,
                                 static_cast<DWORD>(found.size()), found.data(), nullptr);
  return size > 0 && size < found.size()
             ? existing_absolute(std::wstring(found.data(), size))
             : std::wstring{};
}

bool file_exists(const std::wstring& path) noexcept
{
  const DWORD attrs = GetFileAttributesW(path.c_str());
  return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

std::string redact_remote_urls(std::string line)
{
  constexpr std::string_view schemes[] = {
      "http://", "https://", "rtsp://", "rtmp://", "srt://"};
  for (const auto scheme : schemes)
  {
    std::size_t pos = 0;
    while ((pos = line.find(scheme, pos)) != std::string::npos)
    {
      std::size_t end = pos + scheme.size();
      while (end < line.size() && line[end] != ' ' && line[end] != '\t' &&
             line[end] != '\r' && line[end] != '\n' && line[end] != ']' &&
             line[end] != ')')
        ++end;
      line.replace(pos, end - pos, "<redacted-url>");
      pos += sizeof("<redacted-url>") - 1;
    }
  }
  return line;
}

std::vector<std::wstring> build_args(const DesktopSourceConfig& config,
                                     int port, const std::string& key)
{
  const int fps = std::clamp(config.fps, 1, 60);
  const int max_height = std::clamp(config.max_height, 240, 2160);
  std::vector<std::wstring> args{
      L"-hide_banner", L"-nostats", L"-loglevel", L"warning"};

  if (config.kind == DesktopSourceKind::TestPattern)
  {
    args.insert(args.end(), {L"-re", L"-f", L"lavfi", L"-i",
                             L"testsrc2=size=1280x720:rate=" + std::to_wstring(fps)});
  }
  else if (config.kind == DesktopSourceKind::FileOrUrl)
  {
    const std::wstring value = utf8_to_wide(config.value);
    if (value.empty())
      return {};
    args.push_back(L"-re");
    if (file_exists(value))
      args.insert(args.end(), {L"-stream_loop", L"-1"});
    args.insert(args.end(), {L"-i", value});
  }
  else if (config.kind == DesktopSourceKind::Camera)
  {
    const std::wstring name = utf8_to_wide(config.value);
    if (name.empty())
      return {};
    args.insert(args.end(), {L"-f", L"dshow", L"-i", L"video=" + name});
  }
  else
  {
    return {};
  }

  const std::wstring filter = L"scale=-2:min(" + std::to_wstring(max_height) +
                              L"\\,ih),format=yuv420p";
  args.insert(args.end(), {
      L"-map", L"0:v:0", L"-map", L"0:a:0?",
      L"-c:v", L"libx264", L"-preset", L"ultrafast", L"-tune", L"zerolatency",
      L"-bf", L"0", L"-g", L"15", L"-r", std::to_wstring(fps),
      L"-vf", filter,
      L"-c:a", L"aac", L"-ar", L"48000", L"-ac", L"2",
      L"-flvflags", L"no_duration_filesize", L"-flush_packets", L"1",
      L"-f", L"flv",
      L"rtmp://127.0.0.1:" + std::to_wstring(port) + L"/live/" + utf8_to_wide(key)});
  return args;
}

void close_handle(void*& raw) noexcept
{
  if (raw)
  {
    CloseHandle(static_cast<HANDLE>(raw));
    raw = nullptr;
  }
}

} // namespace

DesktopSource::~DesktopSource()
{
  stop();
}

bool DesktopSource::start(const DesktopSourceConfig& config, int rtmp_port,
                          const std::string& stream_key)
{
  stop();
  if (config.kind == DesktopSourceKind::Obs)
    return true;
  if (rtmp_port < 1 || rtmp_port > 65535 || stream_key.empty())
    return false;

  const std::wstring ffmpeg = find_ffmpeg();
  const auto args = build_args(config, rtmp_port, stream_key);
  if (ffmpeg.empty() || args.empty())
  {
    std::lock_guard<std::mutex> lk(mu_);
    status_ = {config.kind, false, ERROR_FILE_NOT_FOUND,
               ffmpeg.empty() ? "ffmpeg.exe not found (set LONGVC_FFMPEG)"
                               : "source value is empty or invalid"};
    cr::log::warn("source", status_.detail);
    return false;
  }

  std::wstring command = join_command(ffmpeg, args);
  SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
  HANDLE pipe_read = nullptr;
  HANDLE pipe_write = nullptr;
  if (!CreatePipe(&pipe_read, &pipe_write, &sa, 64 * 1024) ||
      !SetHandleInformation(pipe_read, HANDLE_FLAG_INHERIT, 0))
  {
    if (pipe_read)
      CloseHandle(pipe_read);
    if (pipe_write)
      CloseHandle(pipe_write);
    return false;
  }

  HANDLE null_in = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (null_in == INVALID_HANDLE_VALUE)
  {
    CloseHandle(pipe_read);
    CloseHandle(pipe_write);
    return false;
  }
  STARTUPINFOEXW si{};
  si.StartupInfo.cb = sizeof(si);
  si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  si.StartupInfo.hStdInput = null_in;
  si.StartupInfo.hStdOutput = pipe_write;
  si.StartupInfo.hStdError = pipe_write;

  SIZE_T attribute_bytes = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_bytes);
  si.lpAttributeList = attribute_bytes
                           ? reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
                                 HeapAlloc(GetProcessHeap(), 0, attribute_bytes))
                           : nullptr;
  HANDLE inherited[] = {null_in, pipe_write};
  const bool attributes_initialized = si.lpAttributeList &&
                                      InitializeProcThreadAttributeList(
                                          si.lpAttributeList, 1, 0, &attribute_bytes);
  if (!si.lpAttributeList ||
      !attributes_initialized ||
      !UpdateProcThreadAttribute(si.lpAttributeList, 0,
                                 PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                 inherited, sizeof(inherited), nullptr, nullptr))
  {
    if (attributes_initialized)
    {
      DeleteProcThreadAttributeList(si.lpAttributeList);
    }
    if (si.lpAttributeList)
      HeapFree(GetProcessHeap(), 0, si.lpAttributeList);
    CloseHandle(null_in);
    CloseHandle(pipe_read);
    CloseHandle(pipe_write);
    return false;
  }

  PROCESS_INFORMATION pi{};
  const BOOL created = CreateProcessW(ffmpeg.c_str(), command.data(), nullptr, nullptr,
                                      TRUE, CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT |
                                                EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED,
                                      nullptr, nullptr, &si.StartupInfo, &pi);
  const DWORD create_error = created ? ERROR_SUCCESS : GetLastError();
  DeleteProcThreadAttributeList(si.lpAttributeList);
  HeapFree(GetProcessHeap(), 0, si.lpAttributeList);
  CloseHandle(null_in);
  CloseHandle(pipe_write);
  if (!created)
  {
    CloseHandle(pipe_read);
    std::lock_guard<std::mutex> lk(mu_);
    status_ = {config.kind, false, create_error, "cannot start ffmpeg"};
    return false;
  }
  HANDLE job = CreateJobObjectW(nullptr, nullptr);
  if (!job)
  {
    TerminateProcess(pi.hProcess, 1);
    WaitForSingleObject(pi.hProcess, 5000);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(pipe_read);
    return false;
  }
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                               &limits, sizeof(limits)) ||
      !AssignProcessToJobObject(job, pi.hProcess))
  {
    TerminateProcess(pi.hProcess, 1);
    WaitForSingleObject(pi.hProcess, 5000);
    CloseHandle(pi.hThread);
    CloseHandle(job);
    CloseHandle(pi.hProcess);
    CloseHandle(pipe_read);
    return false;
  }
  if (ResumeThread(pi.hThread) == static_cast<DWORD>(-1))
  {
    TerminateJobObject(job, 1);
    WaitForSingleObject(pi.hProcess, 5000);
    CloseHandle(pi.hThread);
    CloseHandle(job);
    CloseHandle(pi.hProcess);
    CloseHandle(pipe_read);
    return false;
  }
  CloseHandle(pi.hThread);

  {
    std::lock_guard<std::mutex> lk(mu_);
    process_ = pi.hProcess;
    job_ = job;
    stderr_read_ = pipe_read;
    status_ = {config.kind, true, STILL_ACTIVE, "ffmpeg source running"};
  }
  stopping_.store(false);
  running_.store(true);
  try
  {
    drain_thread_ = std::thread([this, pipe_read, process = pi.hProcess]
                                { drain_(pipe_read, process); });
  }
  catch (...)
  {
    stop();
    return false;
  }

  cr::log::info("source", "ffmpeg publisher started (input value redacted)");
  return true;
}

void DesktopSource::stop() noexcept
{
  stopping_.store(true);
  HANDLE process = nullptr;
  HANDLE job = nullptr;
  {
    std::lock_guard<std::mutex> lk(mu_);
    process = static_cast<HANDLE>(process_);
    job = static_cast<HANDLE>(job_);
  }

  if (process && WaitForSingleObject(process, 0) == WAIT_TIMEOUT)
  {
    if (!job || !TerminateJobObject(job, 0))
      TerminateProcess(process, 0);
    if (WaitForSingleObject(process, 5000) != WAIT_OBJECT_0)
    {
      TerminateProcess(process, 0);
      WaitForSingleObject(process, 1000);
      if (drain_thread_.joinable())
        CancelSynchronousIo(drain_thread_.native_handle());
    }
  }
  if (drain_thread_.joinable())
    CancelSynchronousIo(drain_thread_.native_handle());
  try
  {
    if (drain_thread_.joinable())
      drain_thread_.join();
  }
  catch (...)
  {
  }

  std::lock_guard<std::mutex> lk(mu_);
  running_.store(false);
  status_.running = false;
  close_handle(stderr_read_);
  close_handle(process_);
  close_handle(job_);
}

DesktopSourceStatus DesktopSource::status() const
{
  std::lock_guard<std::mutex> lk(mu_);
  DesktopSourceStatus out = status_;
  out.running = running_.load();
  return out;
}

std::vector<std::string> DesktopSource::list_cameras() noexcept
{
  std::vector<std::string> cameras;
  try
  {
    ComScope com;
    if (!com.usable())
      return cameras;
    ComPtr<ICreateDevEnum> devices;
    if (FAILED(CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER,
                                IID_ICreateDevEnum,
                                reinterpret_cast<void**>(devices.GetAddressOf()))))
      return cameras;
    ComPtr<IEnumMoniker> enumerator;
    if (devices->CreateClassEnumerator(CLSID_VideoInputDeviceCategory,
                                       enumerator.GetAddressOf(), 0) != S_OK)
      return cameras;

    ComPtr<IMoniker> moniker;
    ULONG fetched = 0;
    while (enumerator->Next(1, moniker.ReleaseAndGetAddressOf(), &fetched) == S_OK)
    {
      ComPtr<IPropertyBag> properties;
      if (SUCCEEDED(moniker->BindToStorage(
              nullptr, nullptr, IID_IPropertyBag,
              reinterpret_cast<void**>(properties.GetAddressOf()))))
      {
        VARIANT value;
        VariantInit(&value);
        if (SUCCEEDED(properties->Read(L"FriendlyName", &value, nullptr)) &&
            value.vt == VT_BSTR && value.bstrVal)
        {
          std::string name = wide_to_utf8(value.bstrVal);
          if (!name.empty() && std::find(cameras.begin(), cameras.end(), name) == cameras.end())
            cameras.push_back(std::move(name));
        }
        VariantClear(&value);
      }
    }
    std::sort(cameras.begin(), cameras.end());
  }
  catch (...)
  {
    cameras.clear();
  }
  return cameras;
}

void DesktopSource::drain_(void* pipe_handle, void* process_handle) noexcept
{
  HANDLE pipe = static_cast<HANDLE>(pipe_handle);
  HANDLE process = static_cast<HANDLE>(process_handle);
  std::array<char, 4096> chunk{};
  std::string pending;
  DWORD read = 0;
  while (ReadFile(pipe, chunk.data(), static_cast<DWORD>(chunk.size()), &read, nullptr) && read)
  {
    try
    {
      pending.append(chunk.data(), read);
      std::size_t newline = 0;
      while ((newline = pending.find('\n')) != std::string::npos)
      {
        std::string line = pending.substr(0, newline);
        pending.erase(0, newline + 1);
        if (!line.empty() && line.back() == '\r')
          line.pop_back();
        if (!line.empty())
          cr::log::info("ffmpeg", redact_remote_urls(std::move(line)));
      }
      if (pending.size() > 16 * 1024)
        pending.erase(0, pending.size() - 16 * 1024);
    }
    catch (...)
    {
      pending.clear();
    }
  }

  DWORD exit_code = 0;
  if (!GetExitCodeProcess(process, &exit_code))
    exit_code = GetLastError();
  running_.store(false);
  try
  {
    std::lock_guard<std::mutex> lk(mu_);
    status_.running = false;
    status_.exit_code = exit_code;
    status_.detail = stopping_.load() ? "ffmpeg source stopped"
                                      : "ffmpeg source exited code=" + std::to_string(exit_code);
    if (!stopping_.load())
      cr::log::warn("source", "ffmpeg publisher exited code=" + std::to_string(exit_code));
  }
  catch (...)
  {
    std::lock_guard<std::mutex> lk(mu_);
    status_.running = false;
    status_.exit_code = exit_code;
    status_.detail.clear();
  }
}

} // namespace cr::source
