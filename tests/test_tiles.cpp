// PNG tiles (T10b): the PNG encoder, tile geometry and sampling (block
// interpolation vs per-pixel locate), off-grid transparency, nodata, wind
// rotation, the periodic seam.
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#if __has_include(<zlib.h>)
#include <zlib.h>
#define HAVE_INFLATE 1
#endif

#include "atmo/colormap.h"
#include "atmo/crc32.h"
#include "atmo/png.h"
#include "atmo/tiles.h"
#include "atmo/variables.h"
#include "check.h"
#include "lambert.h"

using namespace atmo;

namespace
{

constexpr double kPiT = 3.14159265358979323846;
constexpr std::int64_t kRun = 1762300800;

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

std::uint32_t be32(const std::string& s, std::size_t p)
{
  return (static_cast<std::uint32_t>(static_cast<unsigned char>(s[p])) << 24) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(s[p + 1]))
          << 16) |
         (static_cast<std::uint32_t>(static_cast<unsigned char>(s[p + 2]))
          << 8) |
         static_cast<unsigned char>(s[p + 3]);
}

// Parses a PNG written by encode_png_rgba: checks the structure and CRCs
// and returns the RGBA pixels (needs zlib to inflate, else empty).
std::vector<std::uint8_t>
decode_png(const std::string& png, std::uint32_t& w, std::uint32_t& h, bool& ok)
{
  ok = png.compare(0, 8, std::string("\x89PNG\r\n\x1a\n", 8)) == 0;
  std::size_t p = 8;
  std::string idat;
  while (ok && p + 12 <= png.size())
  {
    const std::uint32_t len = be32(png, p);
    const std::string type = png.substr(p + 4, 4);
    const std::string body = png.substr(p + 4, 4 + len);
    ok = ok && crc32(body.data(), body.size()) == be32(png, p + 8 + len);
    if (type == "IHDR")
    {
      w = be32(png, p + 8);
      h = be32(png, p + 12);
      ok = ok && png[p + 16] == 8 && png[p + 17] == 6;
    }
    if (type == "IDAT")
      idat += png.substr(p + 8, len);
    p += 12 + len;
    if (type == "IEND")
      break;
  }
  ok = ok && p == png.size();
  std::vector<std::uint8_t> out;
#ifdef HAVE_INFLATE
  std::vector<std::uint8_t> raw((w * 4 + 1) * h);
  uLongf n = static_cast<uLongf>(raw.size());
  ok = ok &&
       uncompress(
         raw.data(), &n, reinterpret_cast<const Bytef*>(idat.data()),
         static_cast<uLong>(idat.size())) == Z_OK &&
       n == raw.size();
  for (std::uint32_t y = 0; ok && y < h; ++y)
  {
    ok = raw[y * (w * 4 + 1)] == 0;  // filter None
    out.insert(
      out.end(), raw.begin() + y * (w * 4 + 1) + 1,
      raw.begin() + (y + 1) * (w * 4 + 1));
  }
#endif
  return out;
}

// In-memory store: analytic fields sampled at the grid points.
class Store : public FieldStore
{
 public:
  std::shared_ptr<const Grid> g;
  std::map<std::uint16_t, std::pair<FieldInfo, std::vector<float>>> f;

  void add(
    std::uint16_t param,
    std::uint8_t flags,
    const std::function<double(double, double)>& fn)
  {
    FieldInfo fi;
    fi.param_id = param;
    fi.level_type = 103;
    fi.level_value = 10.0f;
    fi.flags = flags;
    std::vector<float> v(g->size());
    for (std::size_t k = 0; k < v.size(); ++k)
      v[k] = static_cast<float>(fn(g->lat_deg()[k], g->lon_deg()[k]));
    f[param] = {fi, std::move(v)};
  }
  std::vector<std::string> models() const override { return {"m"}; }
  std::vector<ModelTime> times(std::string_view) const override
  {
    return {ModelTime{kRun, 0}};
  }
  std::shared_ptr<const Grid> grid(std::string_view model, const ModelTime& t)
    const override
  {
    return model == "m" && t.run_s == kRun && t.fhr == 0 ? g : nullptr;
  }
  std::vector<FieldInfo> fields(std::string_view, const ModelTime&)
    const override
  {
    std::vector<FieldInfo> v;
    for (const auto& [k, x] : f)
      v.push_back(x.first);
    return v;
  }
  float sample(
    std::string_view,
    const ModelTime&,
    const FieldKey&,
    const Grid::Cell&) const override
  {
    return NAN;  // tiles use view()
  }
  class View : public FieldView
  {
   public:
    View(
      std::shared_ptr<const Grid> gr,
      FieldInfo fi,
      const std::vector<float>* v)
        : FieldView(std::move(gr), fi), v_(v)
    {
    }
    double at(std::uint32_t i, std::uint32_t j) const override
    {
      return (*v_)[grid().index(i, j)];
    }

   private:
    const std::vector<float>* v_;
  };
  std::shared_ptr<const FieldView> view(
    std::string_view model,
    const ModelTime& t,
    const FieldKey& k) const override
  {
    if (!grid(model, t))
      return nullptr;
    auto it = f.find(k.param_id);
    if (it == f.end() || it->second.first.level_type != k.level_type)
      return nullptr;
    return std::make_shared<View>(g, it->second.first, &it->second.second);
  }
};

std::shared_ptr<const Grid> latlon_grid(
  double lat0,
  double lon0,
  int ni,
  int nj,
  double d,
  bool periodic = false,
  float rot_deg = 0.0f)
{
  GridIdentity id;
  const std::string c = "ll " + std::to_string(lat0) + std::to_string(lon0) +
                        std::to_string(ni) + std::to_string(rot_deg);
  id.canon.assign(c.begin(), c.end());
  id.ni = static_cast<std::uint32_t>(ni);
  id.nj = static_cast<std::uint32_t>(nj);
  id.periodic_i = periodic;
  GridData gd;
  for (int j = 0; j < nj; ++j)
    for (int i = 0; i < ni; ++i)
    {
      gd.lat_deg.push_back(lat0 + j * d);
      gd.lon_deg.push_back(lon0 + i * d);
    }
  if (rot_deg != 0.0f)
    gd.rot_rad.assign(
      static_cast<std::size_t>(ni * nj),
      rot_deg * static_cast<float>(kPiT) / 180.0f);
  return Grid::create(id, gd);
}

TileRequest req(
  std::uint32_t z,
  std::uint32_t x,
  std::uint32_t y,
  const char* field = "wind_u@10m")
{
  TileRequest r;
  r.model = "m";
  r.field = FieldSpec::parse(field);
  r.run_s = kRun;
  r.z = z;
  r.x = x;
  r.y = y;
  return r;
}

void pixel_latlon(
  const TileRequest& r,
  int px,
  int py,
  double& lat,
  double& lon)
{
  const double n = std::ldexp(1.0, static_cast<int>(r.z));
  const double X = (r.x + (px + 0.5) / 256.0) / n;
  const double Y = (r.y + (py + 0.5) / 256.0) / n;
  lon = X * 360.0 - 180.0;
  lat = std::atan(std::sinh(kPiT * (1 - 2 * Y))) * 180.0 / kPiT;
}

}  // namespace

// ---------------------------------------------------------------------------

TEST(png_structure_and_pixels)
{
  std::vector<std::uint8_t> px(7 * 3 * 4);
  for (std::size_t k = 0; k < px.size(); ++k)
    px[k] = static_cast<std::uint8_t>(k * 37);
  const std::string png = encode_png_rgba(7, 3, px.data());
  std::uint32_t w = 0, h = 0;
  bool ok = false;
  const auto back = decode_png(png, w, h, ok);
  CHECK(ok && w == 7 && h == 3);
#ifdef HAVE_INFLATE
  CHECK(back == px);
#endif
  CHECK(encode_png_rgba(7, 3, px.data()) == png);  // deterministic
  CHECK(
    throws<std::invalid_argument>([&] { encode_png_rgba(0, 3, px.data()); }));
  // A flat 256x256 tile compresses to almost nothing.
  std::vector<std::uint8_t> flat(256 * 256 * 4, 0);
  if (png_compressed())
    CHECK(encode_png_rgba(256, 256, flat.data()).size() < 2000);
}

TEST(tile_values_match_the_field)
{
  Store st;
  st.g = latlon_grid(20.0, -130.0, 71, 31, 1.0);  // 20..50 N, 130..60 W
  st.add(
    var::kWindU, 0, [](double lat, double lon) { return lat + 0.1 * lon; });
  // z4 tile (3, 6): lon -112.5..-90, lat ~31..41: entirely on the grid.
  TileResult::Status s;
  const TileRequest r = req(4, 3, 6);
  const auto v = tile_values(st, r, s);
  CHECK(s == TileResult::Status::ok);
  CHECK(v.size() == 256 * 256);
  double worst = 0.0;
  for (int py = 0; py < 256; ++py)
    for (int px = 0; px < 256; ++px)
    {
      double lat, lon;
      pixel_latlon(r, px, py, lat, lon);
      worst = std::max(worst, std::fabs(v[py * 256 + px] - (lat + 0.1 * lon)));
    }
  CHECK(worst < 0.01);

  // Partly off the grid (z3 tile spanning 45..67 N): north part NaN.
  const auto part = tile_values(st, req(3, 1, 2), s);
  CHECK(s == TileResult::Status::ok);
  int nan = 0, fin = 0;
  for (double x : part)
    (std::isnan(x) ? nan : fin)++;
  CHECK(nan > 0 && fin > 0);
  // Entirely off the grid (over Europe).
  CHECK(tile_values(st, req(4, 8, 5), s).size() == 256 * 256);
  CHECK(s == TileResult::Status::empty);
  // Missing data and bad requests.
  CHECK(tile_values(st, req(4, 3, 6, "wind_v@10m"), s).empty());
  CHECK(s == TileResult::Status::no_field);
  TileRequest late = r;
  late.fhr = 6;
  CHECK(tile_values(st, late, s).empty() && s == TileResult::Status::no_time);
  CHECK(
    throws<std::invalid_argument>([&] { tile_values(st, req(2, 4, 0), s); }));
  CHECK(
    throws<std::invalid_argument>([&] { tile_values(st, req(25, 0, 0), s); }));
  CHECK(
    throws<std::invalid_argument>(
      [&] { tile_values(st, req(4, 3, 6, "wind_u"), s); }));
}

TEST(tile_block_interpolation_matches_per_pixel_on_lambert)
{
  // Lambert 3 km grid (projected): the lattice interpolation must agree
  // with locating every pixel.
  atmo_test::Lambert L{38.5, 38.5, 38.5, -97.5, 6371229.0};
  GridIdentity id;
  const std::string c = "lambert tiles";
  id.canon.assign(c.begin(), c.end());
  id.ni = 400;
  id.nj = 300;
  GridData gd;
  for (auto& p : atmo_test::lambert_grid(L, 400, 300, 3000.0))
  {
    gd.lat_deg.push_back(p.first);
    gd.lon_deg.push_back(p.second);
  }
  Store st;
  st.g = Grid::create(id, gd);
  auto fn = [](double lat, double lon)
  { return 10 * std::sin(lat * 0.3) * std::cos(lon * 0.2); };
  st.add(var::kWindU, 0, fn);
  // z7 tile over the projection centre.
  const TileRequest r = req(7, 29, 48);
  TileResult::Status s;
  const auto v = tile_values(st, r, s);
  CHECK(s == TileResult::Status::ok);
  double worst = 0.0;
  int on = 0;
  for (int py = 0; py < 256; py += 3)
    for (int px = 0; px < 256; px += 3)
    {
      double lat, lon;
      pixel_latlon(r, px, py, lat, lon);
      auto cell = st.g->locate(lat, lon);
      if (!cell)
      {
        CHECK(std::isnan(v[py * 256 + px]));
        continue;
      }
      ++on;
      const double exact =
        st.view("m", {kRun, 0}, {var::kWindU, 103, 10.0f})->sample(*cell);
      worst = std::max(worst, std::fabs(v[py * 256 + px] - exact));
    }
  CHECK(on > 5000);
  CHECK(worst < 1e-3);  // field range +-10
}

TEST(tile_colours_offgrid_nodata_winds_seam)
{
  Store st;
  st.g = latlon_grid(20.0, -130.0, 71, 31, 1.0);
  // Missing (NaN) south of 25 N.
  st.add(
    var::kWindU, 0, [](double lat, double) { return lat < 25 ? NAN : lat; });
  const Colormap cm = Colormap::parse("30:000000,lin,50:ffffff", "ff000080");
  const TileResult t = render_tile(st, req(3, 1, 3), cm);  // 0..41 N
  CHECK(t.status == TileResult::Status::ok);
  std::uint32_t w = 0, h = 0;
  bool ok = false;
  const auto px = decode_png(t.png, w, h, ok);
  CHECK(ok && w == 256 && h == 256);
#ifdef HAVE_INFLATE
  int transparent = 0, nodata = 0, clamped_low = 0;
  for (std::size_t k = 0; k < 256 * 256; ++k)
  {
    const std::uint8_t* p = &px[4 * k];
    transparent += p[3] == 0;
    nodata += p[0] == 255 && p[1] == 0 && p[3] == 128;
    clamped_low += p[0] == 0 && p[3] == 255;  // 25..30 N -> first colour
  }
  CHECK(transparent > 0);  // off the grid: transparent despite nodata
  CHECK(nodata > 0);       // on the grid but missing: nodata colour
  CHECK(clamped_low > 0);
#endif
  // Empty tile: valid, fully transparent PNG.
  const TileResult e = render_tile(st, req(4, 8, 5), cm);
  CHECK(e.status == TileResult::Status::empty);
#ifdef HAVE_INFLATE
  const auto ep = decode_png(e.png, w, h, ok);
  bool all_clear = ok;
  for (std::size_t k = 3; k < ep.size(); k += 4)
    all_clear = all_clear && ep[k] == 0;
  CHECK(all_clear);
#endif

  // Grid-relative winds are rotated to earth-relative per pixel.
  Store rot;
  rot.g = latlon_grid(20.0, -130.0, 71, 31, 1.0, false, 30.0f);
  rot.add(var::kWindU, kFieldGridRelative, [](double, double) { return 10.0; });
  rot.add(var::kWindV, kFieldGridRelative, [](double, double) { return 0.0; });
  TileResult::Status s;
  auto u = tile_values(rot, req(4, 3, 6, "wind_u@10m"), s);
  auto v = tile_values(rot, req(4, 3, 6, "wind_v@10m"), s);
  CHECK_NEAR(u[128 * 256 + 128], 10 * std::cos(kPiT / 6), 1e-4);
  CHECK_NEAR(v[128 * 256 + 128], 10 * std::sin(kPiT / 6), 1e-4);

  // Global periodic grid whose seam cell (359.5 -> 0.5 E) straddles the
  // Greenwich tile boundary: blocks next to it must not interpolate
  // across the seam.  A field varying in longitude shows any error.
  Store gl;
  gl.g = latlon_grid(-89.5, 0.5, 360, 180, 1.0, true);
  auto fglobal = [](double lat, double lon)
  { return lat + 10.0 * std::sin(lon * kPiT / 180.0); };
  gl.add(var::kWindU, 0, fglobal);
  double worst = 0.0;
  int on = 0;
  for (std::uint32_t x : {1u, 2u})  // lon -90..0 and 0..90
  {
    const TileRequest tr = req(2, x, 1);
    auto vals = tile_values(gl, tr, s);
    CHECK(s == TileResult::Status::ok);
    for (int py = 0; py < 256; ++py)
      for (int qx = 0; qx < 256; ++qx)
      {
        double lat, lon;
        pixel_latlon(tr, qx, py, lat, lon);
        const double got = vals[py * 256 + qx];
        CHECK(!std::isnan(got));
        worst = std::max(worst, std::fabs(got - fglobal(lat, lon)));
        ++on;
      }
  }
  CHECK(on == 2 * 256 * 256);
  CHECK(worst < 0.05);
}

int main()
{
  return tst::run_all();
}
