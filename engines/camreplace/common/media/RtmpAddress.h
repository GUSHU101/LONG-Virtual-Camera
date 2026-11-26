#pragma once

#include <string>
#include <string_view>

namespace cr::media
{

struct RtmpAddress
{
  std::string host;
  std::string port;
  std::string app;
  std::string tc_url;
  std::string stream;
};

inline bool parse_rtmp_address(std::string_view url, RtmpAddress& out)
{
  if (!url.starts_with("rtmp://") || url.size() > 512)
    return false;
  for (const unsigned char ch : url)
    if (ch < 0x21 || ch > 0x7e || ch == '#' || ch == '\\')
      return false;

  const auto slash = url.find('/', 7);
  if (slash == std::string_view::npos || slash == 7)
    return false;
  const auto authority = url.substr(7, slash - 7);
  if (authority.find_first_of("@?") != std::string_view::npos)
    return false;
  std::string_view host, port;
  if (authority.front() == '[')
  {
    const auto end = authority.find(']');
    if (end == std::string_view::npos || end == 1)
      return false;
    host = authority.substr(1, end - 1);
    if (end + 1 < authority.size())
    {
      if (authority[end + 1] != ':')
        return false;
      port = authority.substr(end + 2);
      if (port.empty())
        return false;
    }
    if (host.find_first_not_of("0123456789abcdefABCDEF:.") != std::string_view::npos)
      return false;
  }
  else
  {
    const auto colon = authority.find(':');
    host = authority.substr(0, colon);
    if (host.empty() || host.find_first_not_of(
           "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ.-") != std::string_view::npos)
      return false;
    if (colon != std::string_view::npos)
    {
      port = authority.substr(colon + 1);
      if (port.empty())
        return false;
    }
  }
  if (port.empty())
    port = "1935";
  if (port.size() > 5 || port.find_first_not_of("0123456789") != std::string_view::npos)
    return false;
  unsigned port_number = 0;
  for (char ch : port)
    port_number = port_number * 10 + static_cast<unsigned>(ch - '0');
  if (port_number == 0 || port_number > 65535)
    return false;

  const auto resource = url.substr(slash + 1);
  const auto path = resource.substr(0, resource.find('?'));
  if (path.empty() || path.front() == '/' || path.back() == '/')
    return false;
  const auto separator = path.rfind('/');
  const auto app = separator == std::string_view::npos ? std::string_view() : path.substr(0, separator);
  const auto stream = separator == std::string_view::npos ? resource : resource.substr(separator + 1);
  const auto base = url.substr(0, app.empty() ? slash : slash + 1 + app.size());
  out = {std::string(host), std::string(port), std::string(app),
         std::string(base), std::string(stream)};
  return true;
}

} // namespace cr::media
