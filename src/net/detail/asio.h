#pragma once

// The one way fv_net's sources include Asio and Beast (only src/net/*.cpp and src/net/detail/
// may, so fv_net's users compile without them).

#if defined(_WIN32) && !defined(_WIN32_WINNT)
#define _WIN32_WINNT 0x0A00 // Windows 10: no fallbacks for older socket APIs
#endif

#include <boost/asio/bind_executor.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ip/v6_only.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http/empty_body.hpp>
#include <boost/beast/http/error.hpp>
#include <boost/beast/http/parser.hpp>
#include <boost/beast/http/read.hpp>
#include <boost/beast/http/string_body.hpp>

// <windows.h>, which Asio brings in, defines these as empty macros.
#ifdef near
#undef near
#endif
#ifdef far
#undef far
#endif

namespace fv::net::detail {
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = boost::beast::http;
using tcp = boost::asio::ip::tcp;
} // namespace fv::net::detail
