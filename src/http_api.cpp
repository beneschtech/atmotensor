#include "atmo/http_api.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/inotify.h>
#include <unistd.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <optional>
#include <stdexcept>
#include <vector>

#include "atmo/colormap.h"
#include "atmo/tiles.h"

namespace atmo
{
namespace
{

namespace fs = std::filesystem;
using nlohmann::json;

struct HttpError : std::runtime_error
{
  int status;
  std::string code;
  HttpError(int s, std::string c, const std::string& msg)
      : std::runtime_error(msg), status(s), code(std::move(c))
  {
  }
};

fcgi::Response json_response(int status, const json& body)
{
  fcgi::Response r;
  r.status = status;
  r.content_type = "application/json";
  r.body = body.dump();
  return r;
}

fcgi::Response
error_response(int status, const std::string& code, const std::string& msg)
{
  fcgi::Response r =
    json_response(status, json{{"error", {{"code", code}, {"message", msg}}}});
  r.headers.push_back({"Cache-Control", "no-store"});
  return r;
}

double get_double(const std::map<std::string, std::string>& q, const char* key)
{
  auto it = q.find(key);
  if (it == q.end())
  {
    throw HttpError(400, "bad_request", std::string("missing ") + key);
  }
  double v = 0.0;
  const std::string& s = it->second;
  const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
  if (r.ec != std::errc() || r.ptr != s.data() + s.size() || !std::isfinite(v))
  {
    throw HttpError(400, "bad_request", std::string("bad ") + key);
  }
  return v;
}

std::uint64_t get_u64(
  const std::map<std::string, std::string>& q,
  const char* key)
{
  const std::string& s = q.at(key);
  std::uint64_t v = 0;
  const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
  if (r.ec != std::errc() || r.ptr != s.data() + s.size())
  {
    throw HttpError(400, "bad_request", std::string("bad ") + key);
  }
  return v;
}

std::vector<std::string> split_list(const std::string& s)
{
  std::vector<std::string> out;
  std::size_t start = 0;
  while (start <= s.size())
  {
    std::size_t end = s.find(',', start);
    if (end == std::string::npos)
    {
      end = s.size();
    }
    if (end > start)
    {
      out.push_back(s.substr(start, end - start));
    }
    start = end + 1;
  }
  return out;
}

int hexval(char c)
{
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

std::string url_decode(std::string_view s)
{
  std::string out;
  for (std::size_t k = 0; k < s.size(); ++k)
  {
    if (s[k] == '+')
    {
      out.push_back(' ');
    }
    else if (s[k] == '%')
    {
      const int hi = k + 1 < s.size() ? hexval(s[k + 1]) : -1;
      const int lo = k + 2 < s.size() ? hexval(s[k + 2]) : -1;
      if (hi < 0 || lo < 0)
      {
        throw std::invalid_argument("bad percent escape");
      }
      out.push_back(static_cast<char>(hi * 16 + lo));
      k += 2;
    }
    else
    {
      out.push_back(s[k]);
    }
  }
  return out;
}

}  // namespace

std::map<std::string, std::string> parse_query_string(std::string_view qs)
{
  std::map<std::string, std::string> out;
  std::size_t start = 0;
  while (start <= qs.size())
  {
    std::size_t end = qs.find('&', start);
    if (end == std::string_view::npos)
    {
      end = qs.size();
    }
    const std::string_view part = qs.substr(start, end - start);
    if (!part.empty())
    {
      const auto eq = part.find('=');
      const std::string key = url_decode(part.substr(0, eq));
      const std::string val =
        eq == std::string_view::npos ? "" : url_decode(part.substr(eq + 1));
      out[key] = val;
    }
    start = end + 1;
  }
  return out;
}

// ---------------------------------------------------------------------------
// QueryApi
// ---------------------------------------------------------------------------

QueryApi::QueryApi(
  fs::path root,
  SnapshotLoader& loader,
  std::size_t pinned_cache,
  GribCache* grib_cache)
    : root_(std::move(root)),
      loader_(loader),
      pinned_cache_(pinned_cache),
      grib_cache_(grib_cache)
{
}

QueryApi::~QueryApi() = default;

QueryApi::View QueryApi::view(std::uint64_t pinned_seq) const
{
  auto cur = loader_.current();
  std::lock_guard<std::mutex> lk(mu_);
  if (cur && current_.snap != cur)
  {
    current_ =
      View{cur, std::make_shared<SnapshotFieldStore>(cur, grib_cache_), false};
  }
  if (pinned_seq == 0 || (cur && cur->seq() == pinned_seq))
  {
    View v = cur ? current_ : View{};
    v.pinned = pinned_seq != 0;
    return v;
  }
  for (auto it = pinned_.begin(); it != pinned_.end(); ++it)
  {
    if (it->snap->seq() == pinned_seq)
    {
      pinned_.splice(pinned_.begin(), pinned_, it);
      return pinned_.front();
    }
  }
  Manifest m;
  try
  {
    m = read_manifest(root_, pinned_seq);
  }
  catch (const std::exception&)
  {
    throw HttpError(
      404, "unknown_snapshot",
      "snapshot " + std::to_string(pinned_seq) + " is not retained");
  }
  std::shared_ptr<const Snapshot> snap;
  try
  {
    snap = std::make_shared<const Snapshot>(root_, std::move(m), cur.get());
  }
  catch (const std::exception& e)
  {
    throw HttpError(
      410, "snapshot_gone",
      "snapshot " + std::to_string(pinned_seq) +
        " can no longer be opened: " + e.what());
  }
  pinned_.push_front(
    View{snap, std::make_shared<SnapshotFieldStore>(snap, grib_cache_), true});
  while (pinned_.size() > pinned_cache_)
  {
    pinned_.pop_back();
  }
  return pinned_.front();
}

fcgi::Response QueryApi::handle(const fcgi::Request& req) const
{
  try
  {
    std::string path = req.param("DOCUMENT_URI");
    if (path.empty())
    {
      path = req.param("SCRIPT_NAME") + req.param("PATH_INFO");
    }
    const std::string method = req.param("REQUEST_METHOD", "GET");
    std::map<std::string, std::string> q;
    try
    {
      q = parse_query_string(req.param("QUERY_STRING"));
    }
    catch (const std::invalid_argument& e)
    {
      throw HttpError(400, "bad_request", e.what());
    }
    const bool tiles = path.rfind("/v1/tiles/", 0) == 0;
    if (
      path != "/healthz" && path != "/v1/meta" && path != "/v1/point" && !tiles)
    {
      throw HttpError(404, "not_found", "no such endpoint: " + path);
    }
    if (method != "GET" && method != "HEAD")
    {
      throw HttpError(405, "method_not_allowed", method + " not allowed");
    }

    const std::uint64_t pin = q.count("snapshot") ? get_u64(q, "snapshot") : 0;
    if (q.count("snapshot") && pin == 0)
    {
      throw HttpError(400, "bad_request", "bad snapshot");
    }
    const View v = view(pin);

    if (path == "/healthz")
    {
      fcgi::Response r;
      r.content_type = "text/plain";
      r.headers.push_back({"Cache-Control", "no-store"});
      if (!v.snap)
      {
        r.status = 503;
        r.body = "no snapshot\n";
      }
      else
      {
        r.body = "ok snapshot " + std::to_string(v.snap->seq()) + "\n";
      }
      return r;
    }
    if (!v.snap)
    {
      throw HttpError(503, "no_snapshot", "no snapshot loaded yet");
    }

    if (tiles)
    {
      return tile(path, q, req, v);
    }

    json body;
    if (path == "/v1/meta")
    {
      body = meta_json(*v.snap, *v.store);
    }
    else
    {
      PointQuery pq;
      pq.lat_deg = get_double(q, "lat");
      pq.lon_deg = get_double(q, "lon");
      if (!q.count("time"))
      {
        throw HttpError(400, "bad_request", "missing time");
      }
      try
      {
        pq.t_s = parse_iso8601(q.at("time"));
      }
      catch (const std::invalid_argument& e)
      {
        throw HttpError(400, "bad_request", e.what());
      }
      if (q.count("level_hpa"))
      {
        pq.level_hpa = get_double(q, "level_hpa");
      }
      if (q.count("alt_m"))
      {
        pq.alt_m = get_double(q, "alt_m");
      }
      if (!q.count("fields") || split_list(q.at("fields")).empty())
      {
        throw HttpError(400, "bad_request", "missing fields");
      }
      for (const auto& m : q.count("models") ? split_list(q.at("models"))
                                             : std::vector<std::string>{})
      {
        pq.models.push_back(m);
      }
      if (q.count("obs"))
      {
        const std::string& o = q.at("obs");
        if (o != "on" && o != "off")
        {
          throw HttpError(400, "bad_request", "obs must be on or off");
        }
        pq.obs = o == "on";
      }
      if (q.count("obs_radius_km"))
      {
        const double r = get_double(q, "obs_radius_km");
        if (r <= 0.0 || r > 2000.0)
        {
          throw HttpError(400, "bad_request", "obs_radius_km out of range");
        }
        pq.obs_radius_km = static_cast<float>(r);
      }
      if (q.count("as_of"))
      {
        pq.as_of = get_u64(q, "as_of");
      }
      try
      {
        for (const auto& f : split_list(q.at("fields")))
        {
          pq.fields.push_back(FieldSpec::parse(f));
        }
        const PointResult res =
          point(*v.store, &v.snap->obs(), v.snap->sources(), pq);
        body = to_json(res);
      }
      catch (const std::invalid_argument& e)
      {
        throw HttpError(400, "bad_request", e.what());
      }
      json pt{
        {"lat", pq.lat_deg}, {"lon", pq.lon_deg}, {"time", iso8601(pq.t_s)}};
      if (pq.level_hpa)
      {
        pt["level_hpa"] = *pq.level_hpa;
      }
      if (pq.alt_m)
      {
        pt["alt_m"] = *pq.alt_m;
      }
      body["point"] = std::move(pt);
      body["snapshot"] = v.snap->seq();
    }
    fcgi::Response r = json_response(200, body);
    r.headers.push_back({"X-Atmo-Snapshot", std::to_string(v.snap->seq())});
    r.headers.push_back(
      {"Cache-Control", v.pinned ? "public, max-age=86400" : "no-store"});
    if (method == "HEAD")
    {
      r.body.clear();
    }
    return r;
  }
  catch (const HttpError& e)
  {
    return error_response(e.status, e.code, e.what());
  }
}

fcgi::Response QueryApi::tile(
  const std::string& path,
  const std::map<std::string, std::string>& q,
  const fcgi::Request& req,
  const View& v) const
{
  // /v1/tiles/{model}/{field}/{run}/{fhr}/{z}/{x}/{y}.png
  std::vector<std::string> seg;
  std::size_t start = std::string("/v1/tiles/").size();
  while (start <= path.size())
  {
    std::size_t end = path.find('/', start);
    if (end == std::string::npos)
    {
      end = path.size();
    }
    seg.push_back(path.substr(start, end - start));
    start = end + 1;
  }
  if (
    seg.size() != 7 || seg[6].size() < 5 ||
    seg[6].compare(seg[6].size() - 4, 4, ".png") != 0)
  {
    throw HttpError(
      404, "not_found",
      "expected /v1/tiles/{model}/{field}/{run}/{fhr}/{z}/{x}/{y}.png");
  }
  seg[6].resize(seg[6].size() - 4);
  auto number = [](const std::string& s, const char* what)
  {
    std::uint64_t n = 0;
    const auto r = std::from_chars(s.data(), s.data() + s.size(), n);
    if (
      s.empty() || r.ec != std::errc() || r.ptr != s.data() + s.size() ||
      n > 0xffffffffu)
    {
      throw HttpError(400, "bad_request", std::string("bad ") + what);
    }
    return static_cast<std::uint32_t>(n);
  };
  TileRequest tr;
  tr.model = seg[0];
  try
  {
    tr.field = FieldSpec::parse(seg[1]);
  }
  catch (const std::invalid_argument& e)
  {
    throw HttpError(400, "bad_request", e.what());
  }
  if (tr.field.level != FieldSpec::Level::fixed)
  {
    throw HttpError(400, "bad_request", "tile fields need an explicit @level");
  }
  if (seg[2].size() != 10)
  {
    throw HttpError(400, "bad_request", "run must be YYYYmmddHH");
  }
  try
  {
    tr.run_s = parse_iso8601(
      seg[2].substr(0, 4) + "-" + seg[2].substr(4, 2) + "-" +
      seg[2].substr(6, 2) + "T" + seg[2].substr(8, 2) + ":00Z");
  }
  catch (const std::invalid_argument&)
  {
    throw HttpError(400, "bad_request", "run must be YYYYmmddHH");
  }
  tr.fhr = number(seg[3], "fhr");
  tr.z = number(seg[4], "z");
  tr.x = number(seg[5], "x");
  tr.y = number(seg[6], "y");
  if (tr.z > 24 || tr.x >= (1u << tr.z) || tr.y >= (1u << tr.z))
  {
    throw HttpError(400, "bad_request", "bad z/x/y");
  }
  const auto models = v.store->models();
  if (!std::binary_search(models.begin(), models.end(), tr.model))
  {
    throw HttpError(404, "not_found", "unknown model \"" + tr.model + "\"");
  }
  if (!q.count("cmap"))
  {
    throw HttpError(400, "bad_request", "missing cmap");
  }
  Colormap cmap;
  try
  {
    cmap = Colormap::parse(
      q.at("cmap"), q.count("nodata")
                      ? std::optional<std::string_view>(q.at("nodata"))
                      : std::nullopt);
  }
  catch (const std::invalid_argument& e)
  {
    throw HttpError(400, "bad_request", e.what());
  }
  const TileResult res = render_tile(*v.store, tr, cmap);
  if (res.status == TileResult::Status::no_time)
  {
    throw HttpError(404, "no_time", "no data for that run and hour");
  }
  if (res.status == TileResult::Status::no_field)
  {
    throw HttpError(404, "no_field", "field not stored at that time");
  }
  fcgi::Response r;
  r.content_type = "image/png";
  const std::string etag =
    "\"" + key_hex(fnv1a64(res.png.data(), res.png.size())) + "\"";
  r.headers.push_back({"X-Atmo-Snapshot", std::to_string(v.snap->seq())});
  // A (run, fhr) never changes once published, so neither does the tile.
  r.headers.push_back({"Cache-Control", "public, max-age=31536000, immutable"});
  r.headers.push_back({"ETag", etag});
  if (req.param("HTTP_IF_NONE_MATCH") == etag)
  {
    r.status = 304;
    return r;
  }
  if (req.param("REQUEST_METHOD", "GET") != "HEAD")
  {
    r.body = res.png;
  }
  return r;
}

// ---------------------------------------------------------------------------
// SnapshotWatcher
// ---------------------------------------------------------------------------

SnapshotWatcher::SnapshotWatcher(
  fs::path root,
  SnapshotLoader& loader,
  std::function<void(const std::string&)> on_error,
  std::chrono::milliseconds poll)
    : root_(std::move(root)),
      loader_(loader),
      on_error_(std::move(on_error)),
      poll_(poll)
{
}

SnapshotWatcher::~SnapshotWatcher()
{
  stop();
}

void SnapshotWatcher::start()
{
  if (thread_.joinable())
  {
    return;
  }
  int p[2];
  if (::pipe2(p, O_NONBLOCK | O_CLOEXEC) != 0)
  {
    throw std::runtime_error("SnapshotWatcher: pipe failed");
  }
  wake_r_ = p[0];
  wake_w_ = p[1];
  stop_ = false;
  thread_ = std::thread([this] { run(); });
}

void SnapshotWatcher::stop()
{
  if (!thread_.joinable())
  {
    return;
  }
  stop_ = true;
  const char c = 1;
  [[maybe_unused]] ssize_t w = ::write(wake_w_, &c, 1);
  thread_.join();
  ::close(wake_r_);
  ::close(wake_w_);
}

void SnapshotWatcher::run()
{
  auto refresh = [&]
  {
    try
    {
      loader_.refresh();
    }
    catch (const std::exception& e)
    {
      if (on_error_)
      {
        on_error_(e.what());
      }
    }
  };
  int ino = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  int wd = -1;
  refresh();
  while (!stop_)
  {
    if (ino >= 0 && wd < 0)
    {
      // The directory may not exist yet (nothing published); retry.
      wd = ::inotify_add_watch(
        ino, (root_ / "manifest").c_str(), IN_MOVED_TO | IN_CLOSE_WRITE);
    }
    pollfd fds[2] = {{wake_r_, POLLIN, 0}, {ino, POLLIN, 0}};
    const int n =
      ::poll(fds, ino >= 0 && wd >= 0 ? 2 : 1, static_cast<int>(poll_.count()));
    if (stop_)
    {
      break;
    }
    if (n > 0 && (fds[1].revents & POLLIN))
    {
      char buf[4096];
      while (::read(ino, buf, sizeof buf) > 0)
      {
      }
    }
    refresh();  // on an event, and as the polling fallback
  }
  if (ino >= 0)
  {
    ::close(ino);
  }
}

}  // namespace atmo
