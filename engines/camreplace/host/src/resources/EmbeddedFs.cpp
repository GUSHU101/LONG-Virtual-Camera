#include "resources/EmbeddedFs.h"
#include "resources/Resources.h"
#include "util/Log.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <Aclapi.h>
#include <shlobj.h> // SHGetKnownFolderPath
#include "util/WinApiDyn.h"

namespace fs = std::filesystem;

namespace cr::resources
{

namespace
{

constexpr uint64_t kSplitMixIncrement = 0x9E3779B97F4A7C15ull;
std::recursive_mutex g_extraction_mutex;
std::atomic<uint64_t> g_stage_counter{0};

uint64_t splitmix64_next(uint64_t& state) noexcept
{
  state += kSplitMixIncrement;
  uint64_t z = state;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

void secure_zero(std::vector<unsigned char>& bytes) noexcept
{
  volatile unsigned char* p = bytes.data();
  for (size_t i = 0; i < bytes.size(); ++i)
  {
    p[i] = 0;
  }
}

struct SecureBytes
{
  std::vector<unsigned char> v;

  SecureBytes() = default;
  explicit SecureBytes(size_t n) : v(n) {}
  SecureBytes(const SecureBytes&) = delete;
  SecureBytes& operator=(const SecureBytes&) = delete;
  SecureBytes(SecureBytes&&) noexcept = default;
  SecureBytes& operator=(SecureBytes&&) noexcept = default;

  ~SecureBytes() noexcept
  {
    secure_zero(v);
  }
};

void xor_splitmix64_stream(unsigned char* data, size_t size, uint64_t seed) noexcept
{
  uint64_t state = seed;
  uint64_t word = 0;
  for (size_t i = 0; i < size; ++i)
  {
    if ((i & 7u) == 0)
    {
      word = splitmix64_next(state);
    }
    data[i] ^= static_cast<unsigned char>((word >> ((i & 7u) * 8u)) & 0xffu);
  }
}

SecureBytes decode_blob(const Blob& b)
{
  if (b.size != 0 && !b.data)
  {
    throw std::runtime_error("embedded blob data missing");
  }
  if (b.encoded_size < b.size)
  {
    throw std::runtime_error("embedded blob truncated");
  }
  if (b.encrypted && b.seed == 0)
  {
    throw std::runtime_error("embedded blob key missing");
  }

  SecureBytes out(b.size);
  if (b.size != 0)
  {
    std::memcpy(out.v.data(), b.data, b.size);
    if (b.encrypted)
    {
      xor_splitmix64_stream(out.v.data(), out.v.size(), static_cast<uint64_t>(b.seed));
    }
  }
  return out;
}

fs::path local_appdata()
{
  PWSTR wp = nullptr;
  if (cr::winapi::SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &wp) != S_OK)
  {
    // Fallback to %USERPROFILE%\AppData\Local — should never hit.
    const char* up = std::getenv("USERPROFILE");
    if (!up)
      throw std::runtime_error("can't locate LocalAppData");
    return fs::path(up) / "AppData" / "Local";
  }
  fs::path p(wp);
  CoTaskMemFree(wp);
  return p;
}

const fs::path& bin_dir()
{
  static const fs::path cached = local_appdata() / "LONG-Virtual-Camera" / "bin";
  return cached;
}

void configure_trustee(EXPLICIT_ACCESSW& entry, PSID sid,
                       TRUSTEE_TYPE type) noexcept
{
  entry = {};
  entry.grfAccessPermissions = GENERIC_ALL;
  entry.grfAccessMode = SET_ACCESS;
  entry.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
  entry.Trustee.TrusteeForm = TRUSTEE_IS_SID;
  entry.Trustee.TrusteeType = type;
  entry.Trustee.ptstrName = reinterpret_cast<LPWSTR>(sid);
}

void secure_extraction_directory(const fs::path& directory)
{
  fs::create_directories(directory);
  const DWORD attrs = GetFileAttributesW(directory.c_str());
  if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY) ||
      (attrs & FILE_ATTRIBUTE_REPARSE_POINT))
  {
    throw std::runtime_error("unsafe extraction directory");
  }

  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
    throw std::runtime_error("OpenProcessToken failed: " + std::to_string(GetLastError()));

  DWORD token_bytes = 0;
  GetTokenInformation(token, TokenUser, nullptr, 0, &token_bytes);
  std::vector<unsigned char> token_storage(token_bytes);
  if (token_bytes == 0 ||
      !GetTokenInformation(token, TokenUser, token_storage.data(), token_bytes,
                           &token_bytes))
  {
    const DWORD error = GetLastError();
    CloseHandle(token);
    throw std::runtime_error("GetTokenInformation failed: " + std::to_string(error));
  }
  PSID user_sid = reinterpret_cast<TOKEN_USER*>(token_storage.data())->User.Sid;

  std::array<unsigned char, SECURITY_MAX_SID_SIZE> system_storage{};
  std::array<unsigned char, SECURITY_MAX_SID_SIZE> admins_storage{};
  DWORD system_bytes = static_cast<DWORD>(system_storage.size());
  DWORD admins_bytes = static_cast<DWORD>(admins_storage.size());
  if (!CreateWellKnownSid(WinLocalSystemSid, nullptr, system_storage.data(),
                          &system_bytes) ||
      !CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr,
                          admins_storage.data(), &admins_bytes))
  {
    const DWORD error = GetLastError();
    CloseHandle(token);
    throw std::runtime_error("CreateWellKnownSid failed: " + std::to_string(error));
  }

  EXPLICIT_ACCESSW entries[3]{};
  configure_trustee(entries[0], user_sid, TRUSTEE_IS_USER);
  configure_trustee(entries[1], system_storage.data(), TRUSTEE_IS_USER);
  configure_trustee(entries[2], admins_storage.data(), TRUSTEE_IS_GROUP);
  PACL acl = nullptr;
  const DWORD acl_error = SetEntriesInAclW(3, entries, nullptr, &acl);
  if (acl_error != ERROR_SUCCESS)
  {
    CloseHandle(token);
    throw std::runtime_error("SetEntriesInAcl failed: " + std::to_string(acl_error));
  }

  std::wstring mutable_path = directory.wstring();
  const auto security_info = static_cast<SECURITY_INFORMATION>(
      DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION);
  const DWORD set_error = SetNamedSecurityInfoW(
      mutable_path.data(), SE_FILE_OBJECT, security_info,
      nullptr, nullptr, acl, nullptr);
  LocalFree(acl);
  CloseHandle(token);
  if (set_error != ERROR_SUCCESS)
    throw std::runtime_error("SetNamedSecurityInfo failed: " + std::to_string(set_error));
}

bool file_matches(const fs::path& path, const SecureBytes& expected)
{
  std::error_code ec;
  if (!fs::is_regular_file(path, ec) || ec ||
      fs::file_size(path, ec) != expected.v.size() || ec)
    return false;

  std::ifstream in(path, std::ios::binary);
  std::array<unsigned char, 64u * 1024u> chunk{};
  size_t offset = 0;
  while (offset < expected.v.size())
  {
    const size_t want = std::min(chunk.size(), expected.v.size() - offset);
    in.read(reinterpret_cast<char*>(chunk.data()), static_cast<std::streamsize>(want));
    if (!in || static_cast<size_t>(in.gcount()) != want ||
        std::memcmp(chunk.data(), expected.v.data() + offset, want) != 0)
      return false;
    offset += want;
  }
  return true;
}

class StagedFile
{
public:
  explicit StagedFile(const fs::path& destination)
  {
    for (unsigned attempt = 0; attempt < 128; ++attempt)
    {
      path_ = destination;
      path_ += ".tmp." + std::to_string(GetCurrentProcessId()) + "." +
               std::to_string(GetCurrentThreadId()) + "." +
               std::to_string(g_stage_counter.fetch_add(1, std::memory_order_relaxed));
      handle_ = CreateFileW(path_.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                            FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_WRITE_THROUGH,
                            nullptr);
      if (handle_ != INVALID_HANDLE_VALUE)
        return;
      const DWORD error = GetLastError();
      if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS)
        break;
    }
    throw std::runtime_error("cannot create unique extraction staging file");
  }

  ~StagedFile() noexcept
  {
    if (handle_ != INVALID_HANDLE_VALUE)
      CloseHandle(handle_);
    if (!committed_ && !path_.empty())
      DeleteFileW(path_.c_str());
  }
  StagedFile(const StagedFile&) = delete;
  StagedFile& operator=(const StagedFile&) = delete;

  void write(const SecureBytes& bytes)
  {
    size_t offset = 0;
    while (offset < bytes.v.size())
    {
      const DWORD chunk = static_cast<DWORD>(std::min<size_t>(
          bytes.v.size() - offset, 1024u * 1024u));
      DWORD written = 0;
      if (!WriteFile(handle_, bytes.v.data() + offset, chunk, &written, nullptr) ||
          written != chunk)
        throw std::runtime_error("embedded staging write failed");
      offset += written;
    }
    if (!FlushFileBuffers(handle_))
      throw std::runtime_error("embedded staging flush failed");
  }

  void commit(const fs::path& destination)
  {
    if (handle_ != INVALID_HANDLE_VALUE)
    {
      CloseHandle(handle_);
      handle_ = INVALID_HANDLE_VALUE;
    }
    if (!MoveFileExW(path_.c_str(), destination.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
      throw std::runtime_error("atomic resource replace failed: " +
                               std::to_string(GetLastError()));
    }
    committed_ = true;
  }

private:
  fs::path path_;
  HANDLE handle_ = INVALID_HANDLE_VALUE;
  bool committed_ = false;
};

// Write blob to path. Skip the write only when the on-disk file matches the
// embedded blob byte-for-byte — anything cheaper (size + N-byte sniff) has
// burned us when only the middle of a binary changed between builds.
// Reading 200 KB – 1 MB from %LOCALAPPDATA% on startup is cheap; the
// assurance that the extracted file actually matches what's baked into the
// running .exe is worth far more.
void write_blob_if_changed(const Blob& b, const fs::path& dest)
{
  if (b.size == 0)
  {
#if defined(CR_ALLOW_MISSING_RESOURCES)
    cr::log::warn("embed", std::string("embedded blob empty: ") + dest.string());
    return;
#else
    throw std::runtime_error("required embedded blob empty: " + dest.string());
#endif
  }

  SecureBytes expected = decode_blob(b);

  if (file_matches(dest, expected))
    return; // already up-to-date

  // Each caller gets an exclusive sibling staging file. Only a completely
  // written and flushed file is atomically made visible; the known-good old
  // resource is never deleted first when adb or antivirus has it open.
  StagedFile staging(dest);
  staging.write(expected);
  staging.commit(dest);
  if (!file_matches(dest, expected))
    throw std::runtime_error("resource verification failed after replace: " + dest.string());
  cr::log::info("embed", std::string("extracted ") + dest.filename().string() + " (" + std::to_string(b.size) + " bytes)");
}

void remove_file_best_effort(const fs::path& path) noexcept
{
  std::error_code ec;
  fs::remove(path, ec);
  if (ec)
  {
    cr::log::warn("embed", "cleanup failed for " + path.string() + ": " + ec.message());
  }
}

} // namespace

// The output filenames are explicit so resource extraction remains
// independent from generated embed metadata.
fs::path extract_adb_suite()
{
  std::lock_guard<std::recursive_mutex> lock(g_extraction_mutex);
  secure_extraction_directory(bin_dir());

  // ADB suite must be extracted together — adb.exe will fail to start if
  // AdbWinApi.dll is missing alongside it.
  write_blob_if_changed(adb_exe(), bin_dir() / "adb.exe");
  write_blob_if_changed(adb_winapi_dll(), bin_dir() / "AdbWinApi.dll");
  write_blob_if_changed(adb_winusbapi_dll(), bin_dir() / "AdbWinUsbApi.dll");
  write_blob_if_changed(libwinpthread_dll(), bin_dir() / "libwinpthread-1.dll");

  return bin_dir();
}

fs::path extract_android_artifacts()
{
  std::lock_guard<std::recursive_mutex> lock(g_extraction_mutex);
  secure_extraction_directory(bin_dir());

  // Android artifacts (pushed to /data/local/tmp on phone).
  write_blob_if_changed(cr_injector(), bin_dir() / "cr_injector");
  write_blob_if_changed(cr_feed_proc(), bin_dir() / "cr_feed_proc");
  write_blob_if_changed(libcr_hooks(), bin_dir() / "libcr_hooks.so");

  return bin_dir();
}

fs::path extract_all()
{
  std::lock_guard<std::recursive_mutex> lock(g_extraction_mutex);
  extract_adb_suite();
  extract_android_artifacts();
  return bin_dir();
}

void cleanup_android_artifacts() noexcept
{
  try
  {
    std::lock_guard<std::recursive_mutex> lock(g_extraction_mutex);
    remove_file_best_effort(bin_dir() / "cr_injector");
    remove_file_best_effort(bin_dir() / "cr_feed_proc");
    remove_file_best_effort(bin_dir() / "libcr_hooks.so");
  }
  catch (const std::exception& e)
  {
    cr::log::warn("embed", std::string("android artifact cleanup failed: ") + e.what());
  }
  catch (...)
  {
    cr::log::warn("embed", "android artifact cleanup failed");
  }
}

void cleanup_bin_dir() noexcept
{
  try
  {
    std::lock_guard<std::recursive_mutex> lock(g_extraction_mutex);
    cleanup_android_artifacts();
    remove_file_best_effort(bin_dir() / "adb.exe");
    remove_file_best_effort(bin_dir() / "AdbWinApi.dll");
    remove_file_best_effort(bin_dir() / "AdbWinUsbApi.dll");
    remove_file_best_effort(bin_dir() / "libwinpthread-1.dll");

    std::error_code ec;
    fs::remove(bin_dir(), ec);
    if (ec)
    {
      cr::log::warn("embed", "cleanup failed for " + bin_dir().string() + ": " + ec.message());
    }
  }
  catch (const std::exception& e)
  {
    cr::log::warn("embed", std::string("bin cleanup failed: ") + e.what());
  }
  catch (...)
  {
    cr::log::warn("embed", "bin cleanup failed");
  }
}

fs::path adb_exe_path()
{
  return bin_dir() / "adb.exe";
}
fs::path cr_injector_path()
{
  return bin_dir() / "cr_injector";
}
fs::path cr_feed_proc_path()
{
  return bin_dir() / "cr_feed_proc";
}
fs::path libcr_hooks_path()
{
  return bin_dir() / "libcr_hooks.so";
}
} // namespace cr::resources
