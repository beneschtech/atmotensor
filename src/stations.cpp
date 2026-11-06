#include "atmo/stations.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <tuple>

#include "atmo/crc32.h"
#include "fileio.h"

static_assert(
  __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__,
  "station files are little-endian");

namespace atmo
{
namespace
{

constexpr const char* kWho = "StationTable";
constexpr std::size_t kPage = 4096;
constexpr std::uint32_t kVersion = 1;
constexpr char kMagic[8] = {'A', 'T', 'S', 'T', 'A', 'T', '0', '1'};
constexpr std::size_t kMaxName = 255;
constexpr std::size_t kMaxIdLen = 31;
constexpr std::uint16_t kKnownFlags = kStationProvisional | kStationMobile;

enum Sec
{
  kSecStations,
  kSecPeriods,
  kSecIds,
  kSecStationIds,
  kSecStrings,
  kNumSecs
};
constexpr const char* kSecNames[kNumSecs] = {
  "stations", "periods", "ids", "station_ids", "strings"};

struct Section
{
  std::uint64_t off;
  std::uint64_t len;
  std::uint32_t crc;
  std::uint32_t reserved;
};

struct Header
{
  char magic[8];
  std::uint32_t version;
  std::uint32_t flags;  // reserved, 0
  std::uint64_t generation;
  std::uint32_t max_ref;       // high-water mark of [1, 2^31)
  std::uint32_t max_prov_ref;  // high-water mark of [2^31, 2^32), 0 = none
  std::uint64_t n_stations;
  std::uint64_t n_periods;
  std::uint64_t n_ids;
  std::uint64_t strings_bytes;
  Section sec[kNumSecs];
  std::uint8_t reserved1[64];
  std::uint32_t reserved2;
  std::uint32_t crc_header;  // over every byte before this field
};
static_assert(sizeof(Header) == 256, "station Header layout");

// Columns, in file order.
enum SCol
{
  kRef,
  kFlags,
  kFirstPeriod,
  kNPeriods,
  kFirstId,
  kNIds,
  kNameOff,
  kNameLen,
  kMerged,
  kNumSCols
};
constexpr std::size_t kSColSize[kNumSCols] = {4, 2, 4, 4, 4, 4, 4, 4, 4};
enum PCol
{
  kFrom,
  kTo,
  kLat,
  kLon,
  kElev,
  kNumPCols
};
constexpr std::size_t kPColSize[kNumPCols] = {8, 8, 4, 4, 4};
enum ICol
{
  kType,
  kValueOff,
  kValueLen,
  kIdRef,
  kIdFrom,
  kIdTo,
  kNumICols
};
constexpr std::size_t kIColSize[kNumICols] = {2, 4, 4, 4, 8, 8};

struct Layout
{
  std::size_t sec_off[kNumSecs] = {};
  std::size_t sec_len[kNumSecs] = {};
  std::size_t scol[kNumSCols] = {};
  std::size_t pcol[kNumPCols] = {};
  std::size_t icol[kNumICols] = {};
  std::size_t station_ids = 0;
  std::size_t strings = 0;
  std::size_t total = 0;

  // Counts < 2^32, strings < 2^32: nothing can overflow.
  Layout(std::size_t n, std::size_t np, std::size_t ni, std::size_t sbytes)
  {
    std::size_t cur = kPage;
    auto section = [&](std::size_t s, auto&& body)
    {
      sec_off[s] = cur;
      std::size_t c = cur;
      body(c);
      sec_len[s] = c - cur;
      cur = detail::align_up(c, kPage);
    };
    auto columns = [](
                     std::size_t& c, std::size_t* out, const std::size_t* sizes,
                     std::size_t ncols, std::size_t rows)
    {
      for (std::size_t k = 0; k < ncols; ++k)
      {
        c = detail::align_up(c, 8);
        out[k] = c;
        c += rows * sizes[k];
      }
    };
    section(
      kSecStations,
      [&](std::size_t& c) { columns(c, scol, kSColSize, kNumSCols, n); });
    section(
      kSecPeriods,
      [&](std::size_t& c) { columns(c, pcol, kPColSize, kNumPCols, np); });
    section(
      kSecIds,
      [&](std::size_t& c) { columns(c, icol, kIColSize, kNumICols, ni); });
    section(
      kSecStationIds,
      [&](std::size_t& c)
      {
        station_ids = c;
        c += ni * 4;
      });
    section(
      kSecStrings,
      [&](std::size_t& c)
      {
        strings = c;
        c += sbytes;
      });
    total = cur;
  }
};

bool valid_id_value(StationIdType type, std::string_view v)
{
  auto all = [&](auto pred) { return std::all_of(v.begin(), v.end(), pred); };
  auto upper_or_digit = [](char c)
  { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); };
  auto digit = [](char c) { return c >= '0' && c <= '9'; };
  auto graphic = [](char c) { return c > ' ' && c < 127; };
  switch (type)
  {
    case StationIdType::icao:
      return v.size() == 4 && all(upper_or_digit);
    case StationIdType::wmo:
      return v.size() == 5 && all(digit);
    case StationIdType::wigos:
      return !v.empty() && v.size() <= kMaxIdLen && all(graphic) &&
             std::count(v.begin(), v.end(), '-') == 3;
    case StationIdType::platform:
      return !v.empty() && v.size() <= kMaxIdLen && all(graphic);
  }
  return false;
}

bool valid_period(const StationPeriod& p)
{
  return p.valid_from_s < p.valid_to_s && std::isfinite(p.lat_deg) &&
         p.lat_deg >= -90.0f && p.lat_deg <= 90.0f &&
         std::isfinite(p.lon_deg) && p.lon_deg >= -180.0f &&
         p.lon_deg <= 360.0f && std::isfinite(p.elevation_m);
}

// Ordering of identifier bindings in the file.
auto id_key(StationIdType t, std::string_view v, std::int64_t from_s)
{
  return std::make_tuple(static_cast<std::uint16_t>(t), v, from_s);
}

void validate_station(const Station& s)
{
  auto bad = [&](const std::string& why)
  {
    return std::invalid_argument(
      std::string(kWho) + ": station '" + s.name + "': " + why);
  };
  if (s.name.size() > kMaxName)
  {
    throw bad("name longer than 255 bytes");
  }
  if ((s.flags & ~kKnownFlags) != 0)
  {
    throw bad("unknown flags");
  }
  if (s.ids.empty())
  {
    throw bad("needs at least one identifier");
  }
  for (std::size_t k = 0; k < s.ids.size(); ++k)
  {
    const StationIdBinding& b = s.ids[k];
    if (!valid_id_value(b.id.type, b.id.value))
    {
      throw bad("invalid identifier '" + b.id.value + "'");
    }
    if (!(b.valid_from_s < b.valid_to_s))
    {
      throw bad("empty identifier interval for '" + b.id.value + "'");
    }
    for (std::size_t q = 0; q < k; ++q)
    {
      const StationIdBinding& o = s.ids[q];
      if (
        o.id == b.id && o.valid_from_s < b.valid_to_s &&
        b.valid_from_s < o.valid_to_s)
      {
        throw bad("identifier '" + b.id.value + "' bound twice at once");
      }
    }
  }
  const bool needs_position =
    (s.flags & (kStationMobile | kStationProvisional)) == 0;
  if (needs_position && s.periods.empty())
  {
    throw bad("a fixed station needs at least one period");
  }
  for (std::size_t k = 0; k < s.periods.size(); ++k)
  {
    if (!valid_period(s.periods[k]))
    {
      throw bad("invalid period " + std::to_string(k));
    }
    if (k > 0 && s.periods[k - 1].valid_to_s > s.periods[k].valid_from_s)
    {
      throw bad("periods overlap or are out of order");
    }
  }
}

template <class T>
void put_col(
  std::vector<unsigned char>& file,
  std::size_t off,
  const std::vector<T>& v)
{
  if (!v.empty())
  {
    std::memcpy(file.data() + off, v.data(), v.size() * sizeof(T));
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// StationTableBuilder

StationTableBuilder::StationTableBuilder() = default;

StationTableBuilder::StationTableBuilder(const StationTable& previous)
    : generation_(previous.generation() + 1),
      base_max_ref_(previous.max_ref()),
      base_max_prov_ref_(previous.max_provisional_ref()),
      max_ref_(previous.max_ref()),
      max_prov_ref_(previous.max_provisional_ref())
{
  for (std::size_t i = 0; i < previous.size(); ++i)
  {
    Station s = previous.station(i).to_station();
    const std::uint32_t ref = s.ref;
    stations_.emplace(ref, std::move(s));
  }
}

std::uint32_t StationTableBuilder::add(Station s)
{
  if (sealed_)
  {
    throw std::logic_error("StationTableBuilder: already sealed");
  }
  validate_station(s);
  if (s.ref == 0)
  {
    if (s.flags & kStationProvisional)
    {
      if (max_prov_ref_ == std::numeric_limits<std::uint32_t>::max())
      {
        throw std::length_error("StationTableBuilder: out of provisional refs");
      }
      s.ref = max_prov_ref_ == 0 ? kProvisionalRefBase : max_prov_ref_ + 1;
    }
    else
    {
      if (max_ref_ + 1 >= kProvisionalRefBase)
      {
        throw std::length_error("StationTableBuilder: out of refs");
      }
      s.ref = max_ref_ + 1;
    }
  }
  else
  {
    const bool prov = s.ref >= kProvisionalRefBase;
    const std::uint32_t floor = prov ? base_max_prov_ref_ : base_max_ref_;
    if (s.ref <= floor || stations_.count(s.ref) != 0)
    {
      throw std::invalid_argument(
        "StationTableBuilder: ref " + std::to_string(s.ref) +
        " already used or below its range's high-water mark (refs are "
        "never reused)");
    }
  }
  const std::uint32_t ref = s.ref;
  if (ref >= kProvisionalRefBase)
  {
    max_prov_ref_ = std::max(max_prov_ref_, ref);
  }
  else
  {
    max_ref_ = std::max(max_ref_, ref);
  }
  stations_.emplace(ref, std::move(s));
  return ref;
}

void StationTableBuilder::update(Station s)
{
  if (sealed_)
  {
    throw std::logic_error("StationTableBuilder: already sealed");
  }
  auto it = stations_.find(s.ref);
  if (it == stations_.end())
  {
    throw std::out_of_range(
      "StationTableBuilder: no station with ref " + std::to_string(s.ref));
  }
  validate_station(s);
  it->second = std::move(s);
}

const Station* StationTableBuilder::find(std::uint32_t ref) const
{
  auto it = stations_.find(ref);
  return it == stations_.end() ? nullptr : &it->second;
}

std::filesystem::path StationTableBuilder::seal(
  const std::filesystem::path& dir)
{
  if (sealed_)
  {
    throw std::logic_error("StationTableBuilder: already sealed");
  }

  // Flatten: strings, periods, and identifier bindings in file order.
  struct Binding
  {
    StationIdType type;
    std::string_view value;
    std::uint32_t ref;
    std::int64_t from_s, to_s;
    std::uint32_t value_off;
  };
  for (const auto& [r, s] : stations_)
  {
    if (s.merged_into == 0)
    {
      continue;
    }
    auto target = stations_.find(s.merged_into);
    if (
      s.merged_into == r || target == stations_.end() ||
      target->second.merged_into != 0)
    {
      throw std::invalid_argument(
        "StationTableBuilder: station " + std::to_string(r) + " merged_into " +
        std::to_string(s.merged_into) +
        " must name another existing, unmerged station");
    }
  }

  std::string strings;
  std::vector<std::uint32_t> ref, first_period, n_periods, first_id, n_ids;
  std::vector<std::uint32_t> name_off, name_len, merged;
  std::vector<std::uint16_t> flags;
  std::vector<std::int64_t> p_from, p_to;
  std::vector<float> p_lat, p_lon;
  std::vector<float> p_elev;
  std::vector<Binding> bindings;
  auto add_string = [&](std::string_view v)
  {
    if (strings.size() + v.size() > std::numeric_limits<std::uint32_t>::max())
    {
      throw std::length_error("StationTableBuilder: strings too large");
    }
    const auto off = static_cast<std::uint32_t>(strings.size());
    strings.append(v);
    return off;
  };
  for (const auto& [r, s] : stations_)
  {
    ref.push_back(r);
    flags.push_back(s.flags);
    merged.push_back(s.merged_into);
    name_off.push_back(add_string(s.name));
    name_len.push_back(static_cast<std::uint32_t>(s.name.size()));
    first_period.push_back(static_cast<std::uint32_t>(p_from.size()));
    n_periods.push_back(static_cast<std::uint32_t>(s.periods.size()));
    for (const StationPeriod& p : s.periods)
    {
      p_from.push_back(p.valid_from_s);
      p_to.push_back(p.valid_to_s);
      p_lat.push_back(p.lat_deg);
      p_lon.push_back(p.lon_deg);
      p_elev.push_back(p.elevation_m);
    }
    for (const StationIdBinding& b : s.ids)
    {
      const std::uint32_t off = add_string(b.id.value);
      bindings.push_back(
        {b.id.type, b.id.value, r, b.valid_from_s, b.valid_to_s, off});
    }
  }
  std::sort(
    bindings.begin(), bindings.end(),
    [](const Binding& a, const Binding& b)
    {
      return id_key(a.type, a.value, a.from_s) <
             id_key(b.type, b.value, b.from_s);
    });
  for (std::size_t k = 1; k < bindings.size(); ++k)
  {
    const Binding& a = bindings[k - 1];
    const Binding& b = bindings[k];
    if (a.type == b.type && a.value == b.value && a.to_s > b.from_s)
    {
      throw std::invalid_argument(
        "StationTableBuilder: identifier '" + std::string(b.value) +
        "' bound to stations " + std::to_string(a.ref) + " and " +
        std::to_string(b.ref) + " at overlapping times");
    }
  }
  // Per station, the indices of its bindings (in table order).
  std::vector<std::vector<std::uint32_t>> per_station(ref.size());
  for (std::size_t k = 0; k < bindings.size(); ++k)
  {
    const auto pos = std::lower_bound(ref.begin(), ref.end(), bindings[k].ref);
    per_station[static_cast<std::size_t>(pos - ref.begin())].push_back(
      static_cast<std::uint32_t>(k));
  }
  std::vector<std::uint32_t> station_ids;
  for (const auto& list : per_station)
  {
    first_id.push_back(static_cast<std::uint32_t>(station_ids.size()));
    n_ids.push_back(static_cast<std::uint32_t>(list.size()));
    station_ids.insert(station_ids.end(), list.begin(), list.end());
  }
  std::vector<std::uint16_t> id_type;
  std::vector<std::uint32_t> id_off, id_len, id_ref;
  std::vector<std::int64_t> id_from, id_to;
  for (const Binding& b : bindings)
  {
    id_type.push_back(static_cast<std::uint16_t>(b.type));
    id_off.push_back(b.value_off);
    id_len.push_back(static_cast<std::uint32_t>(b.value.size()));
    id_ref.push_back(b.ref);
    id_from.push_back(b.from_s);
    id_to.push_back(b.to_s);
  }

  const Layout L(ref.size(), p_from.size(), bindings.size(), strings.size());
  std::vector<unsigned char> file(L.total, 0);
  put_col(file, L.scol[kRef], ref);
  put_col(file, L.scol[kFlags], flags);
  put_col(file, L.scol[kFirstPeriod], first_period);
  put_col(file, L.scol[kNPeriods], n_periods);
  put_col(file, L.scol[kFirstId], first_id);
  put_col(file, L.scol[kNIds], n_ids);
  put_col(file, L.scol[kNameOff], name_off);
  put_col(file, L.scol[kNameLen], name_len);
  put_col(file, L.scol[kMerged], merged);
  put_col(file, L.pcol[kFrom], p_from);
  put_col(file, L.pcol[kTo], p_to);
  put_col(file, L.pcol[kLat], p_lat);
  put_col(file, L.pcol[kLon], p_lon);
  put_col(file, L.pcol[kElev], p_elev);
  put_col(file, L.icol[kType], id_type);
  put_col(file, L.icol[kValueOff], id_off);
  put_col(file, L.icol[kValueLen], id_len);
  put_col(file, L.icol[kIdRef], id_ref);
  put_col(file, L.icol[kIdFrom], id_from);
  put_col(file, L.icol[kIdTo], id_to);
  put_col(file, L.station_ids, station_ids);
  if (!strings.empty())
  {
    std::memcpy(file.data() + L.strings, strings.data(), strings.size());
  }

  Header h;
  std::memset(&h, 0, sizeof h);
  std::memcpy(h.magic, kMagic, 8);
  h.version = kVersion;
  h.generation = generation_;
  h.max_ref = max_ref_;
  h.max_prov_ref = max_prov_ref_;
  h.n_stations = ref.size();
  h.n_periods = p_from.size();
  h.n_ids = bindings.size();
  h.strings_bytes = strings.size();
  for (std::size_t s = 0; s < kNumSecs; ++s)
  {
    h.sec[s].off = L.sec_off[s];
    h.sec[s].len = L.sec_len[s];
    h.sec[s].crc = crc32(file.data() + L.sec_off[s], L.sec_len[s]);
  }
  h.crc_header = crc32(&h, offsetof(Header, crc_header));
  std::memcpy(file.data(), &h, sizeof h);

  const std::filesystem::path path = dir / StationTable::file_name(generation_);
  detail::publish_file(path, file, kWho);
  sealed_ = true;
  stations_.clear();
  return path;
}

// ---------------------------------------------------------------------------
// StationView

StationView::StationView(const StationTable* table, std::size_t index)
    : table_(table), index_(index)
{
}

std::uint32_t StationView::ref() const
{
  return table_->st_ref_[index_];
}

std::string_view StationView::name() const
{
  return table_->str(
    table_->st_name_off_[index_], table_->st_name_len_[index_]);
}

std::uint16_t StationView::flags() const
{
  return table_->st_flags_[index_];
}

std::uint32_t StationView::merged_into() const
{
  return table_->st_merged_[index_];
}

std::size_t StationView::n_periods() const
{
  return table_->st_n_periods_[index_];
}

StationPeriod StationView::period(std::size_t k) const
{
  if (k >= n_periods())
  {
    throw std::out_of_range("StationView: period index out of range");
  }
  return table_->period_view(table_->st_first_period_[index_] + k);
}

std::optional<StationPeriod> StationView::period_at(std::int64_t t_s) const
{
  for (std::size_t k = 0; k < n_periods(); ++k)
  {
    const StationPeriod p = period(k);
    if (p.valid_from_s <= t_s && t_s < p.valid_to_s)
    {
      return p;
    }
  }
  return std::nullopt;
}

std::size_t StationView::n_ids() const
{
  return table_->st_n_ids_[index_];
}

StationIdView StationView::id(std::size_t k) const
{
  if (k >= n_ids())
  {
    throw std::out_of_range("StationView: id index out of range");
  }
  return table_->id_view(
    table_->station_ids_[table_->st_first_id_[index_] + k]);
}

Station StationView::to_station() const
{
  Station s;
  s.ref = ref();
  s.name = std::string(name());
  s.flags = flags();
  s.merged_into = merged_into();
  for (std::size_t k = 0; k < n_ids(); ++k)
  {
    const StationIdView v = id(k);
    s.ids.push_back(
      {StationId{v.type, std::string(v.value)}, v.valid_from_s, v.valid_to_s});
  }
  for (std::size_t k = 0; k < n_periods(); ++k)
  {
    s.periods.push_back(period(k));
  }
  return s;
}

// ---------------------------------------------------------------------------
// StationTable

std::string StationTable::file_name(std::uint64_t generation)
{
  char buf[40];
  std::snprintf(
    buf, sizeof buf, "stations-%016llx.tbl",
    static_cast<unsigned long long>(generation));
  return buf;
}

std::shared_ptr<const StationTable> StationTable::open(
  const std::filesystem::path& path,
  Verify verify)
{
  return std::shared_ptr<const StationTable>(new StationTable(path, verify));
}

StationView StationTable::station(std::size_t index) const
{
  if (index >= n_stations_)
  {
    throw std::out_of_range("StationTable: station index out of range");
  }
  return StationView(this, index);
}

std::optional<StationView> StationTable::by_ref(std::uint32_t ref) const
{
  const std::uint32_t* end = st_ref_ + n_stations_;
  const std::uint32_t* it = std::lower_bound(st_ref_, end, ref);
  if (it == end || *it != ref)
  {
    return std::nullopt;
  }
  return StationView(this, static_cast<std::size_t>(it - st_ref_));
}

std::vector<StationIdView> StationTable::lookup(
  StationIdType type,
  std::string_view value) const
{
  std::vector<StationIdView> out;
  for (std::size_t k = lower_bound(type, value); k < n_ids_; ++k)
  {
    const StationIdView v = id_view(k);
    if (v.type != type || v.value != value)
    {
      break;
    }
    out.push_back(v);
  }
  return out;
}

std::optional<StationAt> StationTable::lookup(
  StationIdType type,
  std::string_view value,
  std::int64_t t_s) const
{
  for (const StationIdView& v : lookup(type, value))
  {
    if (v.valid_from_s <= t_s && t_s < v.valid_to_s)
    {
      const auto st = by_ref(v.ref);  // exists: checked at open
      return StationAt{*st, st->period_at(t_s)};
    }
  }
  return std::nullopt;
}

StationIdView StationTable::id_view(std::size_t k) const
{
  StationIdView v;
  v.type = static_cast<StationIdType>(id_type_[k]);
  v.value = str(id_value_off_[k], id_value_len_[k]);
  v.ref = id_ref_[k];
  v.valid_from_s = id_from_[k];
  v.valid_to_s = id_to_[k];
  return v;
}

StationPeriod StationTable::period_view(std::size_t k) const
{
  return StationPeriod{p_from_[k], p_to_[k], p_lat_[k], p_lon_[k], p_elev_[k]};
}

std::string_view StationTable::str(std::uint32_t off, std::uint32_t len) const
{
  return std::string_view(strings_ + off, len);
}

std::size_t StationTable::lower_bound(
  StationIdType type,
  std::string_view value) const
{
  std::size_t lo = 0, hi = n_ids_;
  const auto want = std::make_tuple(static_cast<std::uint16_t>(type), value);
  while (lo < hi)
  {
    const std::size_t mid = lo + (hi - lo) / 2;
    const auto have = std::make_tuple(
      id_type_[mid], str(id_value_off_[mid], id_value_len_[mid]));
    if (have < want)
    {
      lo = mid + 1;
    }
    else
    {
      hi = mid;
    }
  }
  return lo;
}

StationTable::StationTable(const std::filesystem::path& path, Verify verify)
    : path_(path)
{
  auto bad = [&](const std::string& why)
  {
    return std::runtime_error(
      std::string(kWho) + ": " + path.string() + ": " + why);
  };
  const auto file = detail::MappedFile::open(path, kPage, kWho);
  map_ = file;
  const unsigned char* base = file->data();
  const std::size_t fsize = file->size();

  Header h;
  std::memcpy(&h, base, sizeof h);
  if (std::memcmp(h.magic, kMagic, 8) != 0)
  {
    throw bad("bad magic");
  }
  if (crc32(&h, offsetof(Header, crc_header)) != h.crc_header)
  {
    throw bad("header checksum mismatch");
  }
  if (h.version != kVersion)
  {
    throw bad("unsupported version");
  }
  const std::uint64_t kMax32 = std::numeric_limits<std::uint32_t>::max();
  if (
    h.n_stations > kMax32 || h.n_periods > kMax32 || h.n_ids > kMax32 ||
    h.strings_bytes > kMax32 || h.strings_bytes > fsize)
  {
    throw bad("bad counts");
  }
  generation_ = h.generation;
  max_ref_ = h.max_ref;
  max_prov_ref_ = h.max_prov_ref;
  if (
    max_ref_ >= kProvisionalRefBase ||
    (max_prov_ref_ != 0 && max_prov_ref_ < kProvisionalRefBase))
  {
    throw bad("bad ref high-water marks");
  }
  header_crc_ = h.crc_header;
  n_stations_ = static_cast<std::size_t>(h.n_stations);
  n_periods_ = static_cast<std::size_t>(h.n_periods);
  n_ids_ = static_cast<std::size_t>(h.n_ids);
  strings_len_ = static_cast<std::size_t>(h.strings_bytes);
  const Layout L(n_stations_, n_periods_, n_ids_, strings_len_);
  for (std::size_t s = 0; s < kNumSecs; ++s)
  {
    if (h.sec[s].off != L.sec_off[s] || h.sec[s].len != L.sec_len[s])
    {
      throw bad("bad section offsets");
    }
  }
  if (fsize != L.total)
  {
    throw bad("file size does not match header (truncated?)");
  }
  if (verify == Verify::full)
  {
    for (std::size_t s = 0; s < kNumSecs; ++s)
    {
      if (crc32(base + L.sec_off[s], L.sec_len[s]) != h.sec[s].crc)
      {
        throw bad(std::string(kSecNames[s]) + " section checksum mismatch");
      }
    }
  }

  auto u32 = [&](std::size_t off)
  { return reinterpret_cast<const std::uint32_t*>(base + off); };
  auto i64 = [&](std::size_t off)
  { return reinterpret_cast<const std::int64_t*>(base + off); };
  st_ref_ = u32(L.scol[kRef]);
  st_flags_ = reinterpret_cast<const std::uint16_t*>(base + L.scol[kFlags]);
  st_first_period_ = u32(L.scol[kFirstPeriod]);
  st_n_periods_ = u32(L.scol[kNPeriods]);
  st_first_id_ = u32(L.scol[kFirstId]);
  st_n_ids_ = u32(L.scol[kNIds]);
  st_name_off_ = u32(L.scol[kNameOff]);
  st_name_len_ = u32(L.scol[kNameLen]);
  st_merged_ = u32(L.scol[kMerged]);
  p_from_ = i64(L.pcol[kFrom]);
  p_to_ = i64(L.pcol[kTo]);
  p_lat_ = reinterpret_cast<const float*>(base + L.pcol[kLat]);
  p_lon_ = reinterpret_cast<const float*>(base + L.pcol[kLon]);
  p_elev_ = reinterpret_cast<const float*>(base + L.pcol[kElev]);
  id_type_ = reinterpret_cast<const std::uint16_t*>(base + L.icol[kType]);
  id_value_off_ = u32(L.icol[kValueOff]);
  id_value_len_ = u32(L.icol[kValueLen]);
  id_ref_ = u32(L.icol[kIdRef]);
  id_from_ = i64(L.icol[kIdFrom]);
  id_to_ = i64(L.icol[kIdTo]);
  station_ids_ = u32(L.station_ids);
  strings_ = reinterpret_cast<const char*>(base + L.strings);

  // Structure, so no accessor can read outside the mapping and lookups are
  // well defined.
  auto str_ok = [&](std::uint32_t off, std::uint32_t len)
  { return off <= strings_len_ && len <= strings_len_ - off; };
  std::uint64_t next_period = 0, next_id = 0;
  for (std::size_t i = 0; i < n_stations_; ++i)
  {
    const std::uint32_t r = st_ref_[i];
    const bool in_range =
      r >= kProvisionalRefBase ? r <= max_prov_ref_ : (r != 0 && r <= max_ref_);
    if (!in_range || (i > 0 && r <= st_ref_[i - 1]))
    {
      throw bad("station refs not ascending within their ranges");
    }
    if ((st_flags_[i] & ~kKnownFlags) != 0)
    {
      throw bad("unknown station flags");
    }
    if (st_first_period_[i] != next_period || st_first_id_[i] != next_id)
    {
      throw bad("station slices not contiguous");
    }
    next_period += st_n_periods_[i];
    next_id += st_n_ids_[i];
    if (next_period > n_periods_ || next_id > n_ids_)
    {
      throw bad("station slices out of range");
    }
    if (st_name_len_[i] > kMaxName || !str_ok(st_name_off_[i], st_name_len_[i]))
    {
      throw bad("bad station name");
    }
    for (std::uint32_t k = 0; k < st_n_periods_[i]; ++k)
    {
      const std::size_t p = st_first_period_[i] + k;
      if (!valid_period(period_view(p)) || (k > 0 && p_to_[p - 1] > p_from_[p]))
      {
        throw bad("bad station period");
      }
    }
  }
  if (next_period != n_periods_ || next_id != n_ids_)
  {
    throw bad("slice totals do not match counts");
  }
  for (std::size_t i = 0; i < n_stations_; ++i)
  {
    const std::uint32_t m = st_merged_[i];
    if (m == 0)
    {
      continue;
    }
    const auto target = by_ref(m);
    if (m == st_ref_[i] || !target || target->merged_into() != 0)
    {
      throw bad("bad merged_into");
    }
  }
  for (std::size_t k = 0; k < n_ids_; ++k)
  {
    if (
      id_type_[k] < 1 || id_type_[k] > 4 ||
      !str_ok(id_value_off_[k], id_value_len_[k]) ||
      id_value_len_[k] > kMaxIdLen || !(id_from_[k] < id_to_[k]))
    {
      throw bad("bad identifier binding");
    }
    if (!by_ref(id_ref_[k]))
    {
      throw bad("identifier bound to unknown station");
    }
    if (k > 0)
    {
      const StationIdView a = id_view(k - 1), b = id_view(k);
      if (!(id_key(a.type, a.value, a.valid_from_s) <
            id_key(b.type, b.value, b.valid_from_s)))
      {
        throw bad("identifiers not sorted");
      }
      if (
        a.type == b.type && a.value == b.value && a.valid_to_s > b.valid_from_s)
      {
        throw bad("identifier bound twice at once");
      }
    }
  }
  std::vector<bool> seen(n_ids_, false);
  for (std::size_t i = 0; i < n_stations_; ++i)
  {
    for (std::uint32_t k = 0; k < st_n_ids_[i]; ++k)
    {
      const std::uint32_t idx = station_ids_[st_first_id_[i] + k];
      if (idx >= n_ids_ || seen[idx] || id_ref_[idx] != st_ref_[i])
      {
        throw bad("bad station identifier index");
      }
      seen[idx] = true;
    }
  }
}

}  // namespace atmo
