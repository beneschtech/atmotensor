#include "atmo/colormap.h"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace atmo
{
namespace
{

[[noreturn]] void bad(const std::string& why)
{
  throw std::invalid_argument("cmap: " + why);
}

int hex(char c)
{
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

std::vector<std::string_view> split(std::string_view s, char sep)
{
  std::vector<std::string_view> out;
  std::size_t start = 0;
  for (;;)
  {
    const std::size_t p = s.find(sep, start);
    out.push_back(s.substr(start, p - start));
    if (p == std::string_view::npos)
    {
      return out;
    }
    start = p + 1;
  }
}

std::uint8_t mix(std::uint8_t a, std::uint8_t b, double t)
{
  const double v = a + (static_cast<double>(b) - a) * t;
  return static_cast<std::uint8_t>(
    std::lround(std::fmin(255.0, std::fmax(0.0, v))));
}

}  // namespace

Rgba parse_rgba(std::string_view h)
{
  if (h.size() != 6 && h.size() != 8)
  {
    throw std::invalid_argument(
      "colour \"" + std::string(h) + "\": expected RRGGBB or RRGGBBAA");
  }
  Rgba c{0, 0, 0, 255};
  for (std::size_t k = 0; k < h.size() / 2; ++k)
  {
    const int hi = hex(h[2 * k]), lo = hex(h[2 * k + 1]);
    if (hi < 0 || lo < 0)
    {
      throw std::invalid_argument(
        "colour \"" + std::string(h) + "\": not hexadecimal");
    }
    c[k] = static_cast<std::uint8_t>(hi * 16 + lo);
  }
  return c;
}

Colormap Colormap::parse(
  std::string_view cmap,
  std::optional<std::string_view> nodata)
{
  if (cmap.empty())
  {
    bad("empty");
  }
  if (cmap.size() > 4096)
  {
    bad("too long");
  }
  const auto parts = split(cmap, ',');
  if (parts.size() % 2 == 0)
  {
    bad("expected stop,type,stop,...");
  }
  Colormap m;
  for (std::size_t k = 0; k < parts.size(); ++k)
  {
    const std::string_view p = parts[k];
    if (k % 2 == 1)
    {
      if (p == "lin")
        m.segments_.push_back(Segment::lin);
      else if (p == "log")
        m.segments_.push_back(Segment::log);
      else if (p == "exp")
        m.segments_.push_back(Segment::exp);
      else if (p == "step")
        m.segments_.push_back(Segment::step);
      else
        bad(
          "unknown segment type \"" + std::string(p) +
          "\" (lin, log, exp, step)");
      continue;
    }
    const auto colon = p.find(':');
    if (colon == std::string_view::npos)
    {
      bad("stop \"" + std::string(p) + "\": expected value:RRGGBB[AA]");
    }
    double v = 0.0;
    const std::string_view num = p.substr(0, colon);
    const auto r = std::from_chars(num.data(), num.data() + num.size(), v);
    if (
      num.empty() || r.ec != std::errc() || r.ptr != num.data() + num.size() ||
      !std::isfinite(v))
    {
      bad("stop \"" + std::string(p) + "\": bad value");
    }
    if (!m.values_.empty() && !(v > m.values_.back()))
    {
      bad("stop values must increase");
    }
    try
    {
      m.colors_.push_back(parse_rgba(p.substr(colon + 1)));
    }
    catch (const std::invalid_argument& e)
    {
      bad(e.what());
    }
    m.values_.push_back(v);
  }
  if (m.values_.size() < 2)
  {
    bad("need at least two stops");
  }
  if (m.values_.size() > kMaxStops)
  {
    bad("more than " + std::to_string(kMaxStops) + " stops");
  }
  if (nodata)
  {
    try
    {
      m.nodata_ = parse_rgba(*nodata);
    }
    catch (const std::invalid_argument& e)
    {
      throw std::invalid_argument(std::string("nodata: ") + e.what());
    }
  }
  return m;
}

Rgba Colormap::color(double v) const
{
  if (std::isnan(v))
  {
    return nodata_;
  }
  if (v <= values_.front())
  {
    return colors_.front();
  }
  if (v >= values_.back())
  {
    return colors_.back();
  }
  // Few stops: a linear scan is as fast as a binary search.
  std::size_t k = 0;
  while (v >= values_[k + 1])
  {
    ++k;
  }
  const double u = (v - values_[k]) / (values_[k + 1] - values_[k]);
  double t = u;
  switch (segments_[k])
  {
    case Segment::lin:
      break;
    case Segment::log:
      t = std::log10(1.0 + 9.0 * u);
      break;
    case Segment::exp:
      t = (std::pow(10.0, u) - 1.0) / 9.0;
      break;
    case Segment::step:
      t = 0.0;
      break;
  }
  const Rgba& a = colors_[k];
  const Rgba& b = colors_[k + 1];
  return Rgba{
    mix(a[0], b[0], t), mix(a[1], b[1], t), mix(a[2], b[2], t),
    mix(a[3], b[3], t)};
}

std::string Colormap::to_string() const
{
  static const char* names[] = {"lin", "log", "exp", "step"};
  std::string out;
  for (std::size_t k = 0; k < values_.size(); ++k)
  {
    if (k > 0)
    {
      out += ',';
      out += names[static_cast<int>(segments_[k - 1])];
      out += ',';
    }
    char num[32];
    const auto r = std::to_chars(num, num + sizeof num, values_[k]);
    out.append(num, r.ptr);
    char col[16];
    std::snprintf(
      col, sizeof col, ":%02x%02x%02x%02x", colors_[k][0], colors_[k][1],
      colors_[k][2], colors_[k][3]);
    out += col;
  }
  return out;
}

}  // namespace atmo
