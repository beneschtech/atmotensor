// FastCGI responder (T10; 04-query-api.md "FastCGI front end"): FastCGI
// 1.0, responder role only, no external library.
//
//   Connection  per-connection protocol state machine, no I/O: feed bytes,
//               take completed requests and the protocol replies to send.
//   Server      unix socket and/or 127.0.0.1 listener; one poll() I/O
//               thread plus a worker pool with a bounded queue.
//
// nginx sends one request at a time per connection, so multiplexing is
// refused: GET_VALUES answers FCGI_MPXS_CONNS=0, and a BEGIN_REQUEST for a
// second id while one is active gets END_REQUEST(CANT_MPX_CONN).
// FCGI_KEEP_CONN is honoured.  ABORT_REQUEST drops a request that has not
// been handed out yet (END_REQUEST is sent); one already handed out is
// marked aborted() so only END_REQUEST is sent for it.  The Server does
// not read a connection while its request is being handled, so an abort
// arriving then is seen afterwards and ignored (the response is complete).
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace atmo::fcgi
{

// Record types.
constexpr std::uint8_t kBeginRequest = 1;
constexpr std::uint8_t kAbortRequest = 2;
constexpr std::uint8_t kEndRequest = 3;
constexpr std::uint8_t kParams = 4;
constexpr std::uint8_t kStdin = 5;
constexpr std::uint8_t kStdout = 6;
constexpr std::uint8_t kStderr = 7;
constexpr std::uint8_t kData = 8;
constexpr std::uint8_t kGetValues = 9;
constexpr std::uint8_t kGetValuesResult = 10;
constexpr std::uint8_t kUnknownType = 11;
// Roles, flags, protocol status.
constexpr std::uint16_t kResponder = 1;
constexpr std::uint8_t kKeepConn = 1;
constexpr std::uint8_t kRequestComplete = 0;
constexpr std::uint8_t kCantMpxConn = 1;
constexpr std::uint8_t kOverloaded = 2;
constexpr std::uint8_t kUnknownRole = 3;

struct Limits
{
  std::size_t max_params = 64 * 1024;  // bytes of PARAMS stream
  std::size_t max_body = 1024 * 1024;  // bytes of STDIN stream
  std::size_t max_conns = 256;         // reported via GET_VALUES
  std::size_t max_reqs = 256;
};

struct Request
{
  std::uint16_t id = 0;
  bool keep_conn = false;
  std::map<std::string, std::string> params;
  std::string body;
  // Set if the PARAMS or STDIN stream exceeded the limits (the excess was
  // discarded); the application should answer 431 / 413.
  bool params_too_large = false;
  bool body_too_large = false;

  // params[name] or `def`.
  std::string param(std::string_view name, std::string_view def = {}) const;
};

class Connection
{
 public:
  explicit Connection(Limits limits = {});

  // Consumes bytes.  false = malformed stream (wrong version, bad
  // BEGIN_REQUEST body, bad name/value encoding): close the connection.
  bool feed(const void* data, std::size_t n);

  // The completed request, if any (one at a time; the next one is parsed
  // only after finish()).
  std::optional<Request> take_request();

  // Protocol replies to write (GET_VALUES_RESULT, UNKNOWN_TYPE,
  // END_REQUEST for refused or aborted requests).
  std::string take_output();

  // The handed-out request was aborted by the web server.
  bool aborted() const { return aborted_; }
  // Call after responding to the handed-out request; returns whether the
  // connection should stay open (FCGI_KEEP_CONN).
  bool finish();

 private:
  bool record(std::uint8_t type, std::uint16_t id, std::string_view content);

  Limits limits_;
  std::string in_;       // unparsed bytes
  std::string out_;      // replies
  std::string params_;   // PARAMS stream of the active request
  bool active_ = false;  // a BEGIN_REQUEST is open
  bool handed_out_ = false;
  bool aborted_ = false;
  bool params_done_ = false;
  Request cur_;
  std::optional<Request> ready_;
};

// Name/value pairs (exposed for tests): lengths < 128 in one byte, else
// four bytes with the high bit set.
void append_name_value(
  std::string& out,
  std::string_view name,
  std::string_view value);
bool parse_name_values(
  std::string_view data,
  std::map<std::string, std::string>& out);

// One record (content split by the caller to <= 65535 bytes).
std::string
encode_record(std::uint8_t type, std::uint16_t id, std::string_view content);
std::string encode_end_request(
  std::uint16_t id,
  std::uint32_t app_status,
  std::uint8_t protocol_status);

struct Response
{
  int status = 200;
  std::string content_type = "application/json";
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;
};

// "Status: 200 OK\r\nContent-Type: ...\r\n...\r\n\r\n" + body as STDOUT
// records, the empty STDOUT and END_REQUEST.  With `headers_only` (aborted
// request) only END_REQUEST is produced.
std::string
encode_response(std::uint16_t id, const Response& r, bool headers_only = false);
const char* status_text(int status);

using Handler = std::function<Response(const Request&)>;

struct ServerOptions
{
  std::string unix_path;  // empty = no unix socket
  unsigned unix_mode = 0660;
  bool tcp = false;            // listen on 127.0.0.1
  std::uint16_t tcp_port = 0;  // 0 = ephemeral (see Server::tcp_port())
  std::size_t threads = 4;
  std::size_t queue = 64;                     // waiting requests; beyond -> 503
  std::chrono::milliseconds deadline{10000};  // queue wait; beyond -> 504
  Limits limits;
};

class Server
{
 public:
  // The handler runs on worker threads and must be thread-safe; an
  // exception from it becomes a 500.
  Server(ServerOptions options, Handler handler);
  ~Server();  // stop()
  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;

  // Binds and starts the threads.  std::runtime_error if binding fails.
  void start();
  // Stops accepting, lets queued and running requests finish, closes
  // every connection, joins the threads.  Idempotent.
  void stop();

  std::uint16_t tcp_port() const;
  // Requests answered (any status), for tests and stats.
  std::size_t served() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace atmo::fcgi
