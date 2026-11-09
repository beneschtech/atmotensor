// Colormaps for PNG tiles (T10b, Q12).  Colormaps belong to users and
// products in the web app; they travel in each tile request, so the engine
// keeps no colormap state and a tile URL fully describes its image.
//
// Syntax (query parameter `cmap`):
//     stop ("," type "," stop)+        2 .. 64 stops, values increasing
//     stop  = value ":" RRGGBB[AA]      value in the field's SI units
//     type  = lin | log | exp | step    how the segment between two stops
//                                       is coloured
// e.g. 233.15:2c0f60,lin,273.15:ffffff,lin,313.15:ffef9c
//
// Within a segment [a, b] with u = (v - a) / (b - a):
//     lin   t = u
//     log   t = log10(1 + 9u)     resolves the low end of the segment
//     exp   t = (10^u - 1) / 9    resolves the high end (inverse of log)
//     step  t = 0                 the segment's start colour throughout
// and the colour is the start colour mixed with the end colour by t, per
// RGBA channel.  Values below the first stop get the first colour, above
// the last stop the last colour (Chris: if someone wants something
// different, they put it in their colormap).  Missing values (NaN) get
// `nodata` (parameter `nodata=RRGGBB[AA]`), transparent by default.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace atmo
{

using Rgba = std::array<std::uint8_t, 4>;

class Colormap
{
 public:
  enum class Segment
  {
    lin,
    log,
    exp,
    step,
  };

  static constexpr std::size_t kMaxStops = 64;

  // std::invalid_argument with a message for any syntax or range error.
  static Colormap parse(
    std::string_view cmap,
    std::optional<std::string_view> nodata = {});

  Rgba color(double v) const;

  // Canonical text (the `cmap` syntax), e.g. for logs and ETags.
  std::string to_string() const;
  std::size_t stops() const { return values_.size(); }

 private:
  std::vector<double> values_;
  std::vector<Rgba> colors_;
  std::vector<Segment> segments_;  // stops - 1
  Rgba nodata_{0, 0, 0, 0};
};

// "RRGGBB" or "RRGGBBAA" (hex, either case); std::invalid_argument else.
Rgba parse_rgba(std::string_view hex);

}  // namespace atmo
