// Colormaps (T10b, Q12): syntax, segment types, clamping, nodata.
#include <cmath>
#include <stdexcept>
#include <string>

#include "atmo/colormap.h"
#include "check.h"

using namespace atmo;

namespace
{

template <class E, class F>
bool throws(F&& f)
{
  try
  {
    f();
  }
  catch (const E&)
  {
    return true;
  }
  catch (...)
  {
    return false;
  }
  return false;
}

bool eq(Rgba a, Rgba b)
{
  return a == b;
}

}  // namespace

TEST(colormap_segments_clamp_and_nodata)
{
  const Colormap m = Colormap::parse("0:000000,lin,100:ffffff");
  CHECK(m.stops() == 2);
  CHECK(eq(m.color(0), Rgba{0, 0, 0, 255}));
  CHECK(eq(m.color(100), Rgba{255, 255, 255, 255}));
  CHECK(eq(m.color(50), Rgba{128, 128, 128, 255}));  // 127.5 rounds up
  // Clamped at both ends (Q12): end colours, never transparent.
  CHECK(eq(m.color(-1e9), Rgba{0, 0, 0, 255}));
  CHECK(eq(m.color(1e9), Rgba{255, 255, 255, 255}));
  // Missing: transparent by default, or the nodata colour.
  CHECK(eq(m.color(NAN), Rgba{0, 0, 0, 0}));
  CHECK(eq(
    Colormap::parse("0:000000,lin,1:ffffff", "ff00ff").color(NAN),
    Rgba{255, 0, 255, 255}));
  CHECK(eq(
    Colormap::parse("0:000000,lin,1:ffffff", "ff00ff80").color(NAN),
    Rgba{255, 0, 255, 128}));

  // log and exp at the middle of a segment (inverse functions).
  const double tl = std::log10(5.5);              // 0.7404
  const double te = (std::sqrt(10.0) - 1) / 9.0;  // 0.2403
  const auto lg = Colormap::parse("0:000000,log,100:ffffff").color(50);
  const auto ex = Colormap::parse("0:000000,exp,100:ffffff").color(50);
  CHECK(lg[0] == static_cast<std::uint8_t>(std::lround(255 * tl)));
  CHECK(ex[0] == static_cast<std::uint8_t>(std::lround(255 * te)));
  // step: the segment's start colour up to the next stop.
  const Colormap st = Colormap::parse("0:ff0000,step,10:00ff00,step,20:0000ff");
  CHECK(eq(st.color(9.999), Rgba{255, 0, 0, 255}));
  CHECK(eq(st.color(10), Rgba{0, 255, 0, 255}));
  CHECK(eq(st.color(25), Rgba{0, 0, 255, 255}));
  // Alpha interpolates like the other channels; negatives are fine.
  const Colormap a = Colormap::parse("-40:00000000,lin,-20:ff000080");
  CHECK(eq(a.color(-30), Rgba{128, 0, 0, 64}));
  // Mixed segments pick the right one.
  const Colormap mixed =
    Colormap::parse("233.15:2c0f60,lin,273.15:FFFFFF,exp,313.15:ffef9c");
  CHECK(eq(mixed.color(273.15), Rgba{255, 255, 255, 255}));
  CHECK(mixed.color(253.15)[0] > 0x2c && mixed.color(253.15)[0] < 0xff);
  CHECK(
    mixed.to_string() ==
    "233.15:2c0f60ff,lin,273.15:ffffffff,exp,313.15:ffef9cff");
  CHECK(Colormap::parse(mixed.to_string()).to_string() == mixed.to_string());
}

TEST(colormap_limits_and_errors)
{
  std::string ok64 = "0:000000";
  for (int k = 1; k < 64; ++k)
    ok64 += ",lin," + std::to_string(k) + ":ffffff";
  CHECK(Colormap::parse(ok64).stops() == 64);
  CHECK(
    throws<std::invalid_argument>(
      [&] { Colormap::parse(ok64 + ",lin,64:ffffff"); }));  // 65 stops

  for (const char* bad :
       {"", "0:000000", "0:000000,lin", "0:000000,lin,1:ffffff,",
        "0:000000,lin,0:ffffff", "1:000000,lin,0:ffffff",
        "0:000000,cubic,1:ffffff", "0:00000,lin,1:ffffff",
        "0:0000000,lin,1:ffffff", "0:gg0000,lin,1:ffffff",
        "x:000000,lin,1:ffffff", "0000000,lin,1:ffffff",
        "nan:000000,lin,1:ffffff", "0:000000,lin,inf:ffffff",
        "0:000000,,1:ffffff", ":000000,lin,1:ffffff"})
    CHECK(throws<std::invalid_argument>([&] { Colormap::parse(bad); }));
  CHECK(
    throws<std::invalid_argument>(
      [] { Colormap::parse("0:000000,lin,1:ffffff", "12345"); }));
  CHECK(throws<std::invalid_argument>([] { parse_rgba("zzzzzz"); }));
  CHECK(parse_rgba("A0b1C2") == (Rgba{0xa0, 0xb1, 0xc2, 0xff}));
}

int main()
{
  return tst::run_all();
}
