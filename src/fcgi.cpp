#include "atmo/fcgi.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace atmo::fcgi
{
namespace
{

constexpr std::size_t kHeader = 8;
constexpr std::size_t kMaxContent = 65535;
constexpr std::size_t kChunk = 32768;

std::uint16_t be16(const unsigned char* p)
{
  return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}

}  // namespace

std::string Request::param(std::string_view name, std::string_view def) const
{
  auto it = params.find(std::string(name));
  return it == params.end() ? std::string(def) : it->second;
}

// ---------------------------------------------------------------------------
// Encoding
// ---------------------------------------------------------------------------

void append_name_value(
  std::string& out,
  std::string_view name,
  std::string_view value)
{
  for (std::size_t len : {name.size(), value.size()})
  {
    if (len < 128)
    {
      out.push_back(static_cast<char>(len));
    }
    else
    {
      out.push_back(static_cast<char>(((len >> 24) & 0x7f) | 0x80));
      out.push_back(static_cast<char>((len >> 16) & 0xff));
      out.push_back(static_cast<char>((len >> 8) & 0xff));
      out.push_back(static_cast<char>(len & 0xff));
    }
  }
  out.append(name);
  out.append(value);
}

bool parse_name_values(
  std::string_view data,
  std::map<std::string, std::string>& out)
{
  std::size_t p = 0;
  auto length = [&](std::size_t& len) -> bool
  {
    if (p >= data.size())
    {
      return false;
    }
    const auto b = static_cast<unsigned char>(data[p]);
    if ((b & 0x80) == 0)
    {
      len = b;
      p += 1;
      return true;
    }
    if (p + 4 > data.size())
    {
      return false;
    }
    len =
      (static_cast<std::size_t>(b & 0x7f) << 24) |
      (static_cast<std::size_t>(static_cast<unsigned char>(data[p + 1]))
       << 16) |
      (static_cast<std::size_t>(static_cast<unsigned char>(data[p + 2])) << 8) |
      static_cast<std::size_t>(static_cast<unsigned char>(data[p + 3]));
    p += 4;
    return true;
  };
  while (p < data.size())
  {
    std::size_t nl = 0, vl = 0;
    if (
      !length(nl) || !length(vl) || nl > data.size() - p ||
      vl > data.size() - p - nl)
    {
      return false;
    }
    out[std::string(data.substr(p, nl))] = std::string(data.substr(p + nl, vl));
    p += nl + vl;
  }
  return true;
}

std::string
encode_record(std::uint8_t type, std::uint16_t id, std::string_view content)
{
  if (content.size() > kMaxContent)
  {
    throw std::length_error("fcgi: record content too long");
  }
  const std::size_t pad = (8 - content.size() % 8) % 8;
  std::string r;
  r.reserve(kHeader + content.size() + pad);
  r.push_back(1);
  r.push_back(static_cast<char>(type));
  r.push_back(static_cast<char>(id >> 8));
  r.push_back(static_cast<char>(id & 0xff));
  r.push_back(static_cast<char>(content.size() >> 8));
  r.push_back(static_cast<char>(content.size() & 0xff));
  r.push_back(static_cast<char>(pad));
  r.push_back(0);
  r.append(content);
  r.append(pad, '\0');
  return r;
}

std::string encode_end_request(
  std::uint16_t id,
  std::uint32_t app_status,
  std::uint8_t protocol_status)
{
  const char body[8] = {
    static_cast<char>(app_status >> 24),
    static_cast<char>((app_status >> 16) & 0xff),
    static_cast<char>((app_status >> 8) & 0xff),
    static_cast<char>(app_status & 0xff),
    static_cast<char>(protocol_status),
    0,
    0,
    0};
  return encode_record(kEndRequest, id, std::string_view(body, 8));
}

const char* status_text(int status)
{
  switch (status)
  {
    case 200:
      return "OK";
    case 304:
      return "Not Modified";
    case 400:
      return "Bad Request";
    case 404:
      return "Not Found";
    case 405:
      return "Method Not Allowed";
    case 410:
      return "Gone";
    case 413:
      return "Payload Too Large";
    case 431:
      return "Request Header Fields Too Large";
    case 500:
      return "Internal Server Error";
    case 503:
      return "Service Unavailable";
    case 504:
      return "Gateway Timeout";
    default:
      return "Unknown";
  }
}

std::string
encode_response(std::uint16_t id, const Response& r, bool headers_only)
{
  std::string out;
  if (!headers_only)
  {
    std::string payload = "Status: " + std::to_string(r.status) + " " +
                          status_text(r.status) + "\r\n";
    payload += "Content-Type: " + r.content_type + "\r\n";
    for (const auto& [k, v] : r.headers)
    {
      payload += k + ": " + v + "\r\n";
    }
    payload += "\r\n";
    payload += r.body;
    for (std::size_t p = 0; p < payload.size(); p += kChunk)
    {
      out += encode_record(
        kStdout, id,
        std::string_view(payload).substr(
          p, std::min(kChunk, payload.size() - p)));
    }
    out += encode_record(kStdout, id, {});
  }
  out += encode_end_request(id, 0, kRequestComplete);
  return out;
}

// ---------------------------------------------------------------------------
// Connection
// ---------------------------------------------------------------------------

Connection::Connection(Limits limits) : limits_(limits)
{
}

bool Connection::feed(const void* data, std::size_t n)
{
  in_.append(static_cast<const char*>(data), n);
  std::size_t pos = 0;
  bool ok = true;
  while (ok && in_.size() - pos >= kHeader)
  {
    const auto* h = reinterpret_cast<const unsigned char*>(in_.data() + pos);
    if (h[0] != 1)
    {
      ok = false;
      break;
    }
    const std::size_t clen = be16(h + 4);
    const std::size_t total = kHeader + clen + h[6];
    if (in_.size() - pos < total)
    {
      break;
    }
    ok = record(
      h[1], be16(h + 2), std::string_view(in_.data() + pos + kHeader, clen));
    pos += total;
  }
  in_.erase(0, pos);
  return ok;
}

bool Connection::record(
  std::uint8_t type,
  std::uint16_t id,
  std::string_view content)
{
  if (id == 0)
  {
    if (type == kGetValues)
    {
      std::map<std::string, std::string> names;
      if (!parse_name_values(content, names))
      {
        return false;
      }
      std::string reply;
      for (const auto& [name, unused] : names)
      {
        if (name == "FCGI_MAX_CONNS")
        {
          append_name_value(reply, name, std::to_string(limits_.max_conns));
        }
        else if (name == "FCGI_MAX_REQS")
        {
          append_name_value(reply, name, std::to_string(limits_.max_reqs));
        }
        else if (name == "FCGI_MPXS_CONNS")
        {
          append_name_value(reply, name, "0");
        }
      }
      out_ += encode_record(kGetValuesResult, 0, reply);
    }
    else
    {
      const char body[8] = {static_cast<char>(type), 0, 0, 0, 0, 0, 0, 0};
      out_ += encode_record(kUnknownType, 0, std::string_view(body, 8));
    }
    return true;
  }

  switch (type)
  {
    case kBeginRequest:
    {
      if (content.size() != 8)
      {
        return false;
      }
      const auto* b = reinterpret_cast<const unsigned char*>(content.data());
      if (active_)
      {
        if (id != cur_.id)
        {
          out_ += encode_end_request(id, 0, kCantMpxConn);
        }
        return true;
      }
      if (be16(b) != kResponder)
      {
        out_ += encode_end_request(id, 0, kUnknownRole);
        return true;
      }
      active_ = true;
      handed_out_ = false;
      aborted_ = false;
      params_done_ = false;
      params_.clear();
      ready_.reset();
      cur_ = Request{};
      cur_.id = id;
      cur_.keep_conn = (b[2] & kKeepConn) != 0;
      return true;
    }
    case kParams:
      if (!active_ || id != cur_.id || params_done_)
      {
        return true;
      }
      if (content.empty())
      {
        params_done_ = true;
        if (!cur_.params_too_large && !parse_name_values(params_, cur_.params))
        {
          return false;
        }
        params_.clear();
      }
      else if (params_.size() + content.size() <= limits_.max_params)
      {
        params_.append(content);
      }
      else
      {
        cur_.params_too_large = true;
        params_.clear();
      }
      return true;
    case kStdin:
      if (!active_ || id != cur_.id || ready_ || handed_out_)
      {
        return true;
      }
      if (content.empty())
      {
        if (!params_done_)
        {
          return false;  // STDIN ended before PARAMS
        }
        ready_ = std::move(cur_);
      }
      else if (
        !cur_.body_too_large &&
        cur_.body.size() + content.size() <= limits_.max_body)
      {
        cur_.body.append(content);
      }
      else
      {
        cur_.body_too_large = true;
        cur_.body.clear();
      }
      return true;
    case kAbortRequest:
    {
      const std::uint16_t active_id = ready_ ? ready_->id : cur_.id;
      if (!active_ || id != active_id)
      {
        return true;
      }
      if (handed_out_)
      {
        aborted_ = true;
      }
      else
      {
        active_ = false;
        ready_.reset();
        out_ += encode_end_request(id, 0, kRequestComplete);
      }
      return true;
    }
    default:
      return true;  // DATA and anything else: ignored
  }
}

std::optional<Request> Connection::take_request()
{
  if (!ready_ || handed_out_)
  {
    return std::nullopt;
  }
  handed_out_ = true;
  Request r = std::move(*ready_);
  ready_.reset();
  cur_.id = r.id;
  cur_.keep_conn = r.keep_conn;
  return r;
}

std::string Connection::take_output()
{
  std::string o;
  o.swap(out_);
  return o;
}

bool Connection::finish()
{
  const bool keep = cur_.keep_conn;
  active_ = false;
  handed_out_ = false;
  aborted_ = false;
  ready_.reset();
  return keep;
}

// ---------------------------------------------------------------------------
// Server
// ---------------------------------------------------------------------------

namespace
{

// Writes everything to a (non-blocking) socket, waiting for POLLOUT up to
// `timeout_ms` per stall.  false on error or timeout.
bool send_all(int fd, std::string_view data, int timeout_ms = 10000)
{
  while (!data.empty())
  {
    const ssize_t w = ::send(fd, data.data(), data.size(), MSG_NOSIGNAL);
    if (w > 0)
    {
      data.remove_prefix(static_cast<std::size_t>(w));
      continue;
    }
    if (w < 0 && errno == EINTR)
    {
      continue;
    }
    if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
    {
      pollfd p{fd, POLLOUT, 0};
      if (::poll(&p, 1, timeout_ms) <= 0)
      {
        return false;
      }
      continue;
    }
    return false;
  }
  return true;
}

Response error_response(int status, const char* code, const std::string& msg)
{
  Response r;
  r.status = status;
  r.headers.push_back({"Cache-Control", "no-store"});
  if (status == 503)
  {
    r.headers.push_back({"Retry-After", "1"});
  }
  std::string esc;
  for (char c : msg)
  {
    if (c == '"' || c == '\\')
    {
      esc.push_back('\\');
    }
    esc.push_back(c >= 0x20 ? c : ' ');
  }
  r.body = std::string("{\"error\":{\"code\":\"") + code + "\",\"message\":\"" +
           esc + "\"}}";
  return r;
}

}  // namespace

struct Server::Impl
{
  struct Conn
  {
    int fd = -1;
    Connection proto;
    bool busy = false;
    bool broken = false;  // a write failed: close when done
    explicit Conn(int f, Limits l) : fd(f), proto(l) {}
  };
  struct Job
  {
    Conn* conn;
    Request req;
    std::chrono::steady_clock::time_point queued;
  };

  ServerOptions opt;
  Handler handler;
  int unix_fd = -1, tcp_fd = -1, wake_r = -1, wake_w = -1;
  std::uint16_t port = 0;
  bool started = false, stopped = false;
  std::atomic<bool> stopping{false};
  std::atomic<std::size_t> served{0};
  std::thread io;
  std::vector<std::thread> workers;
  std::map<int, std::unique_ptr<Conn>> conns;  // I/O thread only

  std::mutex mu;
  std::condition_variable cv;
  std::deque<Job> queue;
  std::vector<Conn*> done;
  bool workers_stop = false;

  void wake()
  {
    const char c = 1;
    [[maybe_unused]] ssize_t r = ::write(wake_w, &c, 1);
  }

  void close_conn(int fd)
  {
    ::close(fd);
    conns.erase(fd);
  }

  // Hands a parsed request to the workers (or answers 503).  Returns false
  // if the connection was closed.
  bool dispatch(Conn* c)
  {
    if (c->busy)
    {
      return true;
    }
    auto r = c->proto.take_request();
    if (!r)
    {
      return true;
    }
    std::unique_lock<std::mutex> lk(mu);
    if (queue.size() >= opt.queue)
    {
      lk.unlock();
      const bool ok = send_all(
        c->fd, encode_response(
                 r->id, error_response(503, "overloaded", "server busy")));
      ++served;
      if (!c->proto.finish() || !ok)
      {
        close_conn(c->fd);
        return false;
      }
      return true;
    }
    c->busy = true;
    queue.push_back(Job{c, std::move(*r), std::chrono::steady_clock::now()});
    lk.unlock();
    cv.notify_one();
    return true;
  }

  void io_loop()
  {
    std::vector<pollfd> fds;
    std::vector<char> buf(65536);
    for (;;)
    {
      bool any_busy = false;
      for (const auto& [fd, c] : conns)
      {
        any_busy = any_busy || c->busy;
      }
      if (stopping)
      {
        if (unix_fd >= 0)
        {
          ::close(unix_fd);
          unix_fd = -1;
        }
        if (tcp_fd >= 0)
        {
          ::close(tcp_fd);
          tcp_fd = -1;
        }
        std::lock_guard<std::mutex> lk(mu);
        if (!any_busy && queue.empty() && done.empty())
        {
          break;
        }
      }
      fds.clear();
      fds.push_back({wake_r, POLLIN, 0});
      if (unix_fd >= 0)
      {
        fds.push_back({unix_fd, POLLIN, 0});
      }
      if (tcp_fd >= 0)
      {
        fds.push_back({tcp_fd, POLLIN, 0});
      }
      if (!stopping)
      {
        for (const auto& [fd, c] : conns)
        {
          if (!c->busy)
          {
            fds.push_back({fd, POLLIN, 0});
          }
        }
      }
      if (::poll(fds.data(), fds.size(), 200) < 0 && errno != EINTR)
      {
        break;
      }
      for (const pollfd& p : fds)
      {
        if (p.revents == 0)
        {
          continue;
        }
        if (p.fd == wake_r)
        {
          char tmp[256];
          while (::read(wake_r, tmp, sizeof tmp) > 0)
          {
          }
          std::vector<Conn*> finished;
          {
            std::lock_guard<std::mutex> lk(mu);
            finished.swap(done);
          }
          for (Conn* c : finished)
          {
            c->busy = false;
            const bool keep = c->proto.finish();
            if (!keep || c->broken || stopping)
            {
              close_conn(c->fd);
            }
            else
            {
              dispatch(c);  // a request may already be buffered
            }
          }
        }
        else if (p.fd == unix_fd || p.fd == tcp_fd)
        {
          const int fd =
            ::accept4(p.fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
          if (fd < 0)
          {
            continue;
          }
          if (conns.size() >= opt.limits.max_conns)
          {
            ::close(fd);
            continue;
          }
          conns[fd] = std::make_unique<Conn>(fd, opt.limits);
        }
        else
        {
          auto it = conns.find(p.fd);
          if (it == conns.end() || it->second->busy)
          {
            continue;
          }
          Conn* c = it->second.get();
          bool closed = false;
          for (;;)
          {
            const ssize_t n = ::read(c->fd, buf.data(), buf.size());
            if (n > 0)
            {
              if (!c->proto.feed(buf.data(), static_cast<std::size_t>(n)))
              {
                closed = true;
                break;
              }
              continue;
            }
            if (n < 0 && errno == EINTR)
            {
              continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            {
              break;
            }
            closed = true;  // EOF or error
            break;
          }
          const std::string out = c->proto.take_output();
          if (!out.empty() && !send_all(c->fd, out))
          {
            closed = true;
          }
          if (closed)
          {
            close_conn(c->fd);
            continue;
          }
          dispatch(c);
        }
      }
    }
    for (auto& [fd, c] : conns)
    {
      ::close(fd);
    }
    conns.clear();
  }

  void worker()
  {
    for (;;)
    {
      Job j;
      {
        std::unique_lock<std::mutex> lk(mu);
        cv.wait(lk, [&] { return !queue.empty() || workers_stop; });
        if (queue.empty())
        {
          return;
        }
        j = std::move(queue.front());
        queue.pop_front();
      }
      Response resp;
      if (std::chrono::steady_clock::now() - j.queued > opt.deadline)
      {
        resp = error_response(504, "timeout", "request waited too long");
      }
      else if (j.req.params_too_large)
      {
        resp = error_response(431, "params_too_large", "request too large");
      }
      else if (j.req.body_too_large)
      {
        resp = error_response(413, "body_too_large", "request body too large");
      }
      else
      {
        try
        {
          resp = handler(j.req);
        }
        catch (const std::exception& e)
        {
          resp = error_response(500, "internal", e.what());
        }
        catch (...)
        {
          resp = error_response(500, "internal", "unknown error");
        }
      }
      const std::string bytes =
        encode_response(j.req.id, resp, j.conn->proto.aborted());
      if (!send_all(j.conn->fd, bytes))
      {
        j.conn->broken = true;
      }
      ++served;
      {
        std::lock_guard<std::mutex> lk(mu);
        done.push_back(j.conn);
      }
      wake();
    }
  }
};

Server::Server(ServerOptions options, Handler handler)
    : impl_(std::make_unique<Impl>())
{
  if (options.threads == 0 || options.queue == 0)
  {
    throw std::invalid_argument("fcgi: threads and queue must be > 0");
  }
  if (options.unix_path.empty() && !options.tcp)
  {
    throw std::invalid_argument("fcgi: no unix socket and no tcp listener");
  }
  impl_->opt = std::move(options);
  impl_->handler = std::move(handler);
}

Server::~Server()
{
  stop();
}

void Server::start()
{
  Impl& im = *impl_;
  if (im.started)
  {
    return;
  }
  int p[2];
  if (::pipe2(p, O_NONBLOCK | O_CLOEXEC) != 0)
  {
    throw std::runtime_error("fcgi: pipe failed");
  }
  im.wake_r = p[0];
  im.wake_w = p[1];
  auto fail = [&](const std::string& what)
  {
    const std::string err = std::strerror(errno);
    for (int* fd : {&im.unix_fd, &im.tcp_fd, &im.wake_r, &im.wake_w})
    {
      if (*fd >= 0)
      {
        ::close(*fd);
        *fd = -1;
      }
    }
    throw std::runtime_error("fcgi: " + what + ": " + err);
  };
  if (!im.opt.unix_path.empty())
  {
    sockaddr_un a{};
    a.sun_family = AF_UNIX;
    if (im.opt.unix_path.size() >= sizeof a.sun_path)
    {
      fail("unix socket path too long");
    }
    std::memcpy(a.sun_path, im.opt.unix_path.c_str(), im.opt.unix_path.size());
    struct stat st;
    if (::lstat(im.opt.unix_path.c_str(), &st) == 0)
    {
      if (!S_ISSOCK(st.st_mode))
      {
        errno = EEXIST;
        fail(im.opt.unix_path + " exists and is not a socket");
      }
      ::unlink(im.opt.unix_path.c_str());  // stale socket
    }
    im.unix_fd =
      ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (
      im.unix_fd < 0 ||
      ::bind(im.unix_fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 ||
      ::chmod(im.opt.unix_path.c_str(), im.opt.unix_mode) != 0 ||
      ::listen(im.unix_fd, 128) != 0)
    {
      fail("cannot listen on " + im.opt.unix_path);
    }
  }
  if (im.opt.tcp)
  {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(im.opt.tcp_port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    im.tcp_fd =
      ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    const int one = 1;
    if (
      im.tcp_fd < 0 ||
      ::setsockopt(im.tcp_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) !=
        0 ||
      ::bind(im.tcp_fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 ||
      ::listen(im.tcp_fd, 128) != 0)
    {
      fail("cannot listen on 127.0.0.1:" + std::to_string(im.opt.tcp_port));
    }
    socklen_t len = sizeof a;
    ::getsockname(im.tcp_fd, reinterpret_cast<sockaddr*>(&a), &len);
    im.port = ntohs(a.sin_port);
  }
  im.started = true;
  for (std::size_t k = 0; k < im.opt.threads; ++k)
  {
    im.workers.emplace_back([&im] { im.worker(); });
  }
  im.io = std::thread([&im] { im.io_loop(); });
}

void Server::stop()
{
  Impl& im = *impl_;
  if (!im.started || im.stopped)
  {
    return;
  }
  im.stopped = true;
  im.stopping = true;
  im.wake();
  im.io.join();
  {
    std::lock_guard<std::mutex> lk(im.mu);
    im.workers_stop = true;
  }
  im.cv.notify_all();
  for (auto& t : im.workers)
  {
    t.join();
  }
  ::close(im.wake_r);
  ::close(im.wake_w);
  if (!im.opt.unix_path.empty())
  {
    ::unlink(im.opt.unix_path.c_str());
  }
}

std::uint16_t Server::tcp_port() const
{
  return impl_->port;
}

std::size_t Server::served() const
{
  return impl_->served;
}

}  // namespace atmo::fcgi
