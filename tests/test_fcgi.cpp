// FastCGI responder (T10): the test list in 04-query-api.md "FastCGI front
// end" -- record streams split at every byte boundary, name/value
// encodings, multi-record PARAMS/STDIN, padding, keep-alive, abort,
// CANT_MPX_CONN, GET_VALUES, limits, fuzzing -- plus the socket server and,
// when nginx is installed, a request proxied through a real nginx.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "atmo/fcgi.h"
#include "check.h"

using namespace atmo::fcgi;
namespace fs = std::filesystem;

namespace
{

struct TempDir
{
  fs::path p;
  TempDir()
  {
    // Short: unix socket paths are limited to 108 bytes.
    p = fs::path("/tmp") / ("atfcgi" + std::to_string(::getpid()) + "_" +
                            std::to_string(std::random_device{}() % 100000));
    fs::create_directories(p);
  }
  ~TempDir()
  {
    std::error_code ec;
    fs::remove_all(p, ec);
  }
};

std::string begin(std::uint16_t id, bool keep, std::uint16_t role = kResponder)
{
  const char b[8] = {
    static_cast<char>(role >> 8),
    static_cast<char>(role & 0xff),
    static_cast<char>(keep ? kKeepConn : 0),
    0,
    0,
    0,
    0,
    0};
  return encode_record(kBeginRequest, id, std::string_view(b, 8));
}

// A complete request stream with PARAMS and STDIN split into many small
// records.
std::string request_stream(
  std::uint16_t id,
  bool keep,
  const std::map<std::string, std::string>& params,
  const std::string& body,
  std::size_t piece = 7)
{
  std::string pv;
  for (const auto& [k, v] : params)
    append_name_value(pv, k, v);
  std::string s = begin(id, keep);
  for (std::size_t p = 0; p < pv.size(); p += piece)
    s += encode_record(kParams, id, std::string_view(pv).substr(p, piece));
  s += encode_record(kParams, id, {});
  for (std::size_t p = 0; p < body.size(); p += piece)
    s += encode_record(kStdin, id, std::string_view(body).substr(p, piece));
  s += encode_record(kStdin, id, {});
  return s;
}

struct Rec
{
  std::uint8_t type;
  std::uint16_t id;
  std::string content;
};

// Client-side record parser.
std::vector<Rec> parse_records(const std::string& s)
{
  std::vector<Rec> out;
  std::size_t p = 0;
  while (s.size() - p >= 8)
  {
    const auto* h = reinterpret_cast<const unsigned char*>(s.data() + p);
    const std::size_t cl = (h[4] << 8) | h[5];
    out.push_back(
      Rec{
        h[1], static_cast<std::uint16_t>((h[2] << 8) | h[3]),
        s.substr(p + 8, cl)});
    p += 8 + cl + h[6];
  }
  return out;
}

std::uint8_t end_status(const Rec& r)
{
  return static_cast<std::uint8_t>(r.content.at(4));
}

const std::map<std::string, std::string> kParamsBig = {
  {"REQUEST_METHOD", "GET"},
  {"SCRIPT_NAME", "/v1/point"},
  {"QUERY_STRING", "lat=35.4&lon=-97.6&fields=air_temperature@2m"},
  {"LONG_VALUE", std::string(300, 'x')},               // 4-byte length
  {std::string(200, 'N'), "long name, short value"}};  // 4-byte name length

}  // namespace

// ---------------------------------------------------------------------------
// Protocol
// ---------------------------------------------------------------------------

TEST(fcgi_name_value_encodings)
{
  for (std::size_t len :
       {0ul, 1ul, 127ul, 128ul, 255ul, 300000ul, (1ul << 24) + 5})
  {
    std::string enc;
    append_name_value(enc, "k", std::string(len, 'v'));
    std::map<std::string, std::string> m;
    CHECK(parse_name_values(enc, m));
    CHECK(m["k"].size() == len);
    CHECK(enc.size() == 1 + (len < 128 ? 1 : 4) + 1 + len);
  }
  std::string enc;
  append_name_value(enc, "name", "value");
  std::map<std::string, std::string> m;
  for (std::size_t cut = 1; cut < enc.size(); ++cut)
  {
    m.clear();
    CHECK(!parse_name_values(enc.substr(0, cut), m));
  }
  const std::string huge = "\x81\x00\x00\x00\x01";  // claims 16 MiB name
  CHECK(!parse_name_values(huge, m));
}

TEST(fcgi_split_at_every_byte_boundary)
{
  const std::string body = "{\"route\":[1,2,3]}";
  const std::string s = request_stream(1, true, kParamsBig, body);
  auto check_req = [&](const Request& r)
  {
    CHECK(r.id == 1 && r.keep_conn);
    CHECK(r.params == kParamsBig);
    CHECK(r.body == body);
    CHECK(r.param("SCRIPT_NAME") == "/v1/point");
    CHECK(r.param("MISSING", "def") == "def");
    CHECK(!r.params_too_large && !r.body_too_large);
  };
  for (std::size_t cut = 0; cut <= s.size(); ++cut)
  {
    Connection c;
    CHECK(c.feed(s.data(), cut));
    if (cut < s.size())
      CHECK(!c.take_request());
    CHECK(c.feed(s.data() + cut, s.size() - cut));
    auto r = c.take_request();
    CHECK(r.has_value());
    if (r)
      check_req(*r);
    CHECK(c.take_output().empty());
  }
  // One byte at a time.
  Connection c;
  for (char ch : s)
    CHECK(c.feed(&ch, 1));
  auto r = c.take_request();
  CHECK(r.has_value());
  if (r)
    check_req(*r);
}

TEST(fcgi_padding_keepalive_and_sequence)
{
  // Unpadded records (padding 0) are accepted too.
  std::string unpadded;
  {
    std::string pv;
    append_name_value(pv, "A", "B");  // 4 bytes, normally padded to 8
    unpadded = begin(5, false);
    std::string rec = encode_record(kParams, 5, pv);
    rec[6] = 0;  // drop the padding
    rec.resize(8 + pv.size());
    unpadded +=
      rec + encode_record(kParams, 5, {}) + encode_record(kStdin, 5, {});
  }
  Connection c;
  CHECK(c.feed(unpadded.data(), unpadded.size()));
  auto r = c.take_request();
  CHECK(r && r->param("A") == "B" && !r->keep_conn);
  CHECK(!c.finish());  // no FCGI_KEEP_CONN: close

  // Kept-alive: two requests back to back, the second buffered while the
  // first is handled.
  Connection k;
  const std::string two = request_stream(1, true, {{"N", "1"}}, "") +
                          request_stream(2, true, {{"N", "2"}}, "");
  CHECK(k.feed(two.data(), two.size()));
  auto r1 = k.take_request();
  CHECK(r1 && r1->param("N") == "1");
  CHECK(!k.take_request());  // one at a time
  CHECK(k.finish());
  // The second request was refused while the first was active (nginx
  // never does this; a well-behaved client waits).
  const auto out = parse_records(k.take_output());
  CHECK(
    out.size() == 1 && out[0].type == kEndRequest && out[0].id == 2 &&
    end_status(out[0]) == kCantMpxConn);
  // Sent after finish(), it is accepted.
  const std::string second = request_stream(2, true, {{"N", "2"}}, "");
  CHECK(k.feed(second.data(), second.size()));
  auto r2 = k.take_request();
  CHECK(r2 && r2->id == 2 && r2->param("N") == "2");
}

TEST(fcgi_cant_mpx_abort_get_values_roles)
{
  // Second request id while one is active.
  Connection c;
  std::string s = begin(1, true) + begin(2, true);
  CHECK(c.feed(s.data(), s.size()));
  auto out = parse_records(c.take_output());
  CHECK(
    out.size() == 1 && out[0].id == 2 && end_status(out[0]) == kCantMpxConn);
  s = encode_record(kParams, 1, {}) + encode_record(kStdin, 1, {});
  CHECK(c.feed(s.data(), s.size()));
  CHECK(c.take_request().has_value());

  // Abort before the request is complete: dropped, END_REQUEST sent.
  Connection a;
  s = begin(3, true) + encode_record(kAbortRequest, 3, {});
  CHECK(a.feed(s.data(), s.size()));
  out = parse_records(a.take_output());
  CHECK(out.size() == 1 && out[0].type == kEndRequest && out[0].id == 3);
  s = encode_record(kParams, 3, {}) + encode_record(kStdin, 3, {});
  CHECK(a.feed(s.data(), s.size()));
  CHECK(!a.take_request());
  // Abort after hand-out: marked.
  Connection b;
  s = request_stream(4, true, {}, "") + encode_record(kAbortRequest, 4, {});
  CHECK(b.feed(s.data(), s.size()));
  // The abort was parsed before hand-out here (same buffer): dropped.
  CHECK(!b.take_request());
  Connection b2;
  s = request_stream(4, true, {}, "");
  CHECK(b2.feed(s.data(), s.size()));
  CHECK(b2.take_request().has_value());
  s = encode_record(kAbortRequest, 4, {});
  CHECK(b2.feed(s.data(), s.size()));
  CHECK(b2.aborted());
  CHECK(
    encode_response(4, Response{}, true) ==
    encode_end_request(4, 0, kRequestComplete));

  // GET_VALUES.
  Connection g;
  std::string names;
  append_name_value(names, "FCGI_MPXS_CONNS", "");
  append_name_value(names, "FCGI_MAX_REQS", "");
  append_name_value(names, "FCGI_UNKNOWN_THING", "");
  s = encode_record(kGetValues, 0, names);
  CHECK(g.feed(s.data(), s.size()));
  out = parse_records(g.take_output());
  CHECK(out.size() == 1 && out[0].type == kGetValuesResult);
  std::map<std::string, std::string> vals;
  CHECK(!out.empty() && parse_name_values(out[0].content, vals));
  CHECK(vals["FCGI_MPXS_CONNS"] == "0");
  CHECK(vals.count("FCGI_MAX_REQS") == 1);
  CHECK(vals.count("FCGI_UNKNOWN_THING") == 0);
  // Unknown management record type.
  s = encode_record(42, 0, "xx");
  CHECK(g.feed(s.data(), s.size()));
  out = parse_records(g.take_output());
  CHECK(
    out.size() == 1 && out[0].type == kUnknownType &&
    static_cast<unsigned char>(out[0].content[0]) == 42);

  // Role other than responder.
  Connection role;
  s = begin(9, false, 2 /* authorizer */);
  CHECK(role.feed(s.data(), s.size()));
  out = parse_records(role.take_output());
  CHECK(out.size() == 1 && end_status(out[0]) == kUnknownRole);
}

TEST(fcgi_limits_and_malformed_input)
{
  Limits lim;
  lim.max_params = 64;
  lim.max_body = 16;
  Connection c(lim);
  std::string s = request_stream(1, false, kParamsBig, "a body over 16 bytes");
  CHECK(c.feed(s.data(), s.size()));
  auto r = c.take_request();
  CHECK(r && r->params_too_large && r->body_too_large);
  CHECK(r && r->params.empty() && r->body.empty());

  auto rejects = [](const std::string& bytes)
  {
    Connection x;
    return !x.feed(bytes.data(), bytes.size());
  };
  std::string v2 = begin(1, false);
  v2[0] = 2;
  CHECK(rejects(v2));                                              // version
  CHECK(rejects(encode_record(kBeginRequest, 1, "12345")));        // short body
  CHECK(rejects(begin(1, false) + encode_record(kStdin, 1, {})));  // order
  std::string badpv = begin(1, false) + encode_record(kParams, 1, "\x05") +
                      encode_record(kParams, 1, {});
  CHECK(rejects(badpv));  // bad pairs
}

TEST(fcgi_fuzz_never_crashes)
{
  std::mt19937 rng(1234);
  const std::string valid = request_stream(7, true, kParamsBig, "body");
  for (int n = 0; n < 20000; ++n)
  {
    std::string s;
    if (n % 2 == 0)
    {
      std::uniform_int_distribution<int> len(0, 300), byte(0, 255);
      s.resize(static_cast<std::size_t>(len(rng)));
      for (auto& ch : s)
        ch = static_cast<char>(byte(rng));
      if (n % 4 == 0 && !s.empty())
        s[0] = 1;  // valid version byte more often
    }
    else
    {
      s = valid;
      std::uniform_int_distribution<std::size_t> pos(0, s.size() - 1);
      std::uniform_int_distribution<int> byte(0, 255);
      for (int k = 0; k < 3; ++k)
        s[pos(rng)] = static_cast<char>(byte(rng));
      s.resize(pos(rng));
    }
    Connection c;
    std::uniform_int_distribution<std::size_t> cut(0, s.size());
    const std::size_t k = cut(rng);
    if (c.feed(s.data(), k))
      c.feed(s.data() + k, s.size() - k);
    (void)c.take_request();
    (void)c.take_output();
  }
  CHECK(true);
}

TEST(fcgi_encode_response)
{
  Response r;
  r.status = 404;
  r.headers = {{"X-Atmo-Snapshot", "12"}};
  r.body = std::string(100000, 'b');  // several STDOUT records
  const auto recs = parse_records(encode_response(3, r));
  std::string stdout_data;
  std::size_t n_stdout = 0;
  for (const auto& x : recs)
    if (x.type == kStdout)
    {
      stdout_data += x.content;
      ++n_stdout;
    }
  CHECK(n_stdout >= 4);
  CHECK(recs.back().type == kEndRequest && recs.back().id == 3);
  CHECK(
    recs[recs.size() - 2].type == kStdout &&
    recs[recs.size() - 2].content.empty());
  CHECK(
    stdout_data.rfind(
      "Status: 404 Not Found\r\nContent-Type: "
      "application/json\r\nX-Atmo-Snapshot: 12\r\n\r\n",
      0) == 0);
  CHECK(stdout_data.size() == stdout_data.find("\r\n\r\n") + 4 + 100000);
}

// ---------------------------------------------------------------------------
// Server
// ---------------------------------------------------------------------------

namespace
{

int connect_unix(const std::string& path)
{
  const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  sockaddr_un a{};
  a.sun_family = AF_UNIX;
  std::strncpy(a.sun_path, path.c_str(), sizeof a.sun_path - 1);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0)
  {
    ::close(fd);
    return -1;
  }
  return fd;
}

int connect_tcp(std::uint16_t port)
{
  const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0)
  {
    ::close(fd);
    return -1;
  }
  return fd;
}

void send_str(int fd, const std::string& s)
{
  std::size_t p = 0;
  while (p < s.size())
  {
    const ssize_t w = ::send(fd, s.data() + p, s.size() - p, MSG_NOSIGNAL);
    if (w <= 0)
      return;
    p += static_cast<std::size_t>(w);
  }
}

// Reads records until END_REQUEST for `id`; returns the STDOUT data, or
// "<closed>" if the server closed the connection first.
std::string read_response(int fd, std::uint16_t id)
{
  std::string buf, out;
  char tmp[65536];
  for (;;)
  {
    std::size_t p = 0;
    while (buf.size() - p >= 8)
    {
      const auto* h = reinterpret_cast<const unsigned char*>(buf.data() + p);
      const std::size_t cl = (h[4] << 8) | h[5];
      const std::size_t total = 8 + cl + h[6];
      if (buf.size() - p < total)
        break;
      const std::uint16_t rid = static_cast<std::uint16_t>((h[2] << 8) | h[3]);
      if (h[1] == kStdout && rid == id)
        out.append(buf, p + 8, cl);
      if (h[1] == kEndRequest && rid == id)
        return out;
      p += total;
    }
    buf.erase(0, p);
    const ssize_t n = ::recv(fd, tmp, sizeof tmp, 0);
    if (n <= 0)
      return "<closed>";
    buf.append(tmp, static_cast<std::size_t>(n));
  }
}

std::string body_of(const std::string& resp)
{
  const auto p = resp.find("\r\n\r\n");
  return p == std::string::npos ? "" : resp.substr(p + 4);
}

Response echo(const Request& r)
{
  Response resp;
  resp.content_type = "text/plain";
  resp.body = r.param("QUERY_STRING") + "|" + r.body;
  if (r.param("QUERY_STRING") == "throw")
    throw std::runtime_error("boom");
  if (r.param("QUERY_STRING").rfind("sleep", 0) == 0)
    std::this_thread::sleep_for(
      std::chrono::milliseconds(std::stoi(r.param("QUERY_STRING").substr(5))));
  return resp;
}

}  // namespace

TEST(fcgi_server_keepalive_concurrency_errors)
{
  TempDir td;
  ServerOptions o;
  o.unix_path = (td.p / "q.sock").string();
  o.tcp = true;
  o.threads = 4;
  Server srv(o, echo);
  srv.start();
  CHECK(srv.tcp_port() != 0);
  CHECK(
    (fs::status(o.unix_path).permissions() & fs::perms::others_all) ==
    fs::perms::none);

  // Two requests on one kept-alive connection.
  int fd = connect_unix(o.unix_path);
  CHECK(fd >= 0);
  send_str(fd, request_stream(1, true, {{"QUERY_STRING", "a=1"}}, "x"));
  std::string resp = read_response(fd, 1);
  CHECK(resp.rfind("Status: 200 OK\r\nContent-Type: text/plain\r\n", 0) == 0);
  CHECK(body_of(resp) == "a=1|x");
  send_str(fd, request_stream(2, true, {{"QUERY_STRING", "b=2"}}, ""));
  CHECK(body_of(read_response(fd, 2)) == "b=2|");
  // Handler exception -> 500 with a JSON error; connection still usable.
  send_str(fd, request_stream(3, true, {{"QUERY_STRING", "throw"}}, ""));
  resp = read_response(fd, 3);
  CHECK(resp.rfind("Status: 500", 0) == 0);
  CHECK(body_of(resp).find("\"internal\"") != std::string::npos);
  // Without KEEP_CONN the server closes after the response.
  send_str(fd, request_stream(4, false, {{"QUERY_STRING", "c"}}, ""));
  CHECK(body_of(read_response(fd, 4)) == "c|");
  char tmp;
  CHECK(::recv(fd, &tmp, 1, 0) == 0);
  ::close(fd);

  // Garbage: the connection is closed, the server keeps serving.
  fd = connect_unix(o.unix_path);
  send_str(fd, std::string("\x07garbage-garbage", 16));
  CHECK(::recv(fd, &tmp, 1, 0) == 0);
  ::close(fd);

  // Many clients at once, over TCP and the unix socket.
  std::atomic<int> ok{0};
  std::vector<std::thread> clients;
  for (int t = 0; t < 8; ++t)
    clients.emplace_back(
      [&, t]
      {
        const int c =
          t % 2 ? connect_unix(o.unix_path) : connect_tcp(srv.tcp_port());
        for (int n = 0; n < 20; ++n)
        {
          const std::string q =
            "t=" + std::to_string(t) + "&n=" + std::to_string(n);
          const auto id = static_cast<std::uint16_t>(n + 1);
          send_str(c, request_stream(id, true, {{"QUERY_STRING", q}}, ""));
          if (body_of(read_response(c, id)) == q + "|")
            ++ok;
        }
        ::close(c);
      });
  for (auto& c : clients)
    c.join();
  CHECK(ok == 160);
  srv.stop();
  CHECK(!fs::exists(o.unix_path));
}

TEST(fcgi_server_overload_and_deadline)
{
  TempDir td;
  ServerOptions o;
  o.unix_path = (td.p / "q.sock").string();
  o.threads = 1;
  o.queue = 1;
  o.deadline = std::chrono::milliseconds(100);
  Server srv(o, echo);
  srv.start();
  // One slow request running, one queued (it will wait > deadline -> 504),
  // the rest refused at once with 503.
  std::vector<int> fds;
  std::vector<std::string> res(5);
  for (int k = 0; k < 5; ++k)
  {
    fds.push_back(connect_unix(o.unix_path));
    send_str(
      fds.back(), request_stream(1, false, {{"QUERY_STRING", "sleep300"}}, ""));
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
  }
  int n200 = 0, n503 = 0, n504 = 0;
  for (int k = 0; k < 5; ++k)
  {
    const std::string r = read_response(fds[k], 1);
    n200 += r.rfind("Status: 200", 0) == 0;
    n503 += r.rfind("Status: 503", 0) == 0;
    n504 += r.rfind("Status: 504", 0) == 0;
    if (r.rfind("Status: 503", 0) == 0)
      CHECK(r.find("Retry-After: 1") != std::string::npos);
    ::close(fds[k]);
  }
  CHECK(n200 == 1 && n504 == 1 && n503 == 3);
  CHECK(srv.served() == 5);
}

TEST(fcgi_server_stop_drains_in_flight)
{
  TempDir td;
  ServerOptions o;
  o.unix_path = (td.p / "q.sock").string();
  o.threads = 2;
  Server srv(o, echo);
  srv.start();
  const int fd = connect_unix(o.unix_path);
  send_str(fd, request_stream(1, true, {{"QUERY_STRING", "sleep200"}}, ""));
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  std::thread stopper([&] { srv.stop(); });
  const std::string r = read_response(fd, 1);
  stopper.join();
  CHECK(body_of(r) == "sleep200|");
  ::close(fd);
  CHECK(connect_unix(o.unix_path) < 0);  // no longer listening
}

// Through a real nginx, if installed (skipped otherwise).
TEST(fcgi_through_nginx)
{
  if (!fs::exists("/usr/sbin/nginx"))
  {
    std::printf("  (nginx not installed: skipped)\n");
    return;
  }
  TempDir td;
  ServerOptions o;
  o.unix_path = (td.p / "q.sock").string();
  Server srv(
    o,
    [](const Request& r)
    {
      Response resp;
      resp.headers = {{"X-Atmo-Snapshot", "42"}};
      resp.body = "{\"script\":\"" + r.param("SCRIPT_NAME") +
                  "\",\"query\":\"" + r.param("QUERY_STRING") + "\"}";
      return resp;
    });
  srv.start();

  std::mt19937 rng(std::random_device{}());
  const int port = 20000 + static_cast<int>(rng() % 30000);
  const fs::path conf = td.p / "nginx.conf";
  std::ofstream(conf)
    << "worker_processes 1;\npid " << (td.p / "nginx.pid").string() << ";\n"
    << "error_log " << (td.p / "error.log").string() << ";\n"
    << "events { worker_connections 64; }\nhttp {\n"
    << "  access_log off;\n"
    << "  client_body_temp_path " << td.p.string() << "/cb;\n"
    << "  fastcgi_temp_path " << td.p.string() << "/fc;\n"
    << "  proxy_temp_path " << td.p.string() << "/px;\n"
    << "  uwsgi_temp_path " << td.p.string() << "/uw;\n"
    << "  scgi_temp_path " << td.p.string() << "/sc;\n"
    << "  upstream atmo { server unix:" << o.unix_path << "; keepalive 4; }\n"
    << "  server { listen 127.0.0.1:" << port << ";\n"
    << "    location /v1/ {\n"
    << "      fastcgi_param SCRIPT_NAME $fastcgi_script_name;\n"
    << "      fastcgi_param QUERY_STRING $query_string;\n"
    << "      fastcgi_param REQUEST_METHOD $request_method;\n"
    << "      fastcgi_keep_conn on;\n"
    << "      fastcgi_pass atmo;\n    }\n  }\n}\n";
  const pid_t pid = ::fork();
  if (pid == 0)
  {
    ::execl(
      "/usr/sbin/nginx", "nginx", "-p", td.p.c_str(), "-c", conf.c_str(), "-g",
      "daemon off;", static_cast<char*>(nullptr));
    ::_exit(127);
  }
  int fd = -1;
  for (int k = 0; k < 100 && fd < 0; ++k)
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    fd = connect_tcp(static_cast<std::uint16_t>(port));
  }
  CHECK(fd >= 0);
  if (fd >= 0)
  {
    for (int n = 0; n < 2; ++n)  // twice: exercises the kept-alive upstream
    {
      const int c = n == 0 ? fd : connect_tcp(static_cast<std::uint16_t>(port));
      send_str(
        c,
        "GET /v1/point?lat=35&lon=-97 HTTP/1.1\r\nHost: x\r\n"
        "Connection: close\r\n\r\n");
      std::string http;
      char tmp[4096];
      ssize_t r;
      while ((r = ::recv(c, tmp, sizeof tmp, 0)) > 0)
        http.append(tmp, static_cast<std::size_t>(r));
      ::close(c);
      CHECK(http.rfind("HTTP/1.1 200 OK", 0) == 0);
      CHECK(http.find("X-Atmo-Snapshot: 42") != std::string::npos);
      CHECK(http.find("Content-Type: application/json") != std::string::npos);
      CHECK(
        http.find("{\"script\":\"/v1/point\",\"query\":\"lat=35&lon=-97\"}") !=
        std::string::npos);
    }
  }
  ::kill(pid, SIGTERM);
  int status = 0;
  ::waitpid(pid, &status, 0);
  srv.stop();
}

int main()
{
  return tst::run_all();
}
