// Copyright (c) FIRST and other WPILib contributors.
// Open Source Software; you can modify and/or share it under the terms of
// the WPILib BSD license file in the root directory of this project.

#include "wpinet/WebSocket.h"

#include <random>
#include <span>
#include <string>
#include <string_view>

#include <fmt/format.h>
#include <wpi/Base64.h>
#include <wpi/SmallString.h>
#include <wpi/SmallVector.h>
#include <wpi/StringExtras.h>
#include <wpi/raw_ostream.h>
#include <wpi/sha1.h>

#include "WebSocketDebug.h"
#include "WebSocketSerializer.h"
#include "wpinet/HttpParser.h"
#include "wpinet/raw_uv_ostream.h"
#include "wpinet/uv/Stream.h"

using namespace wpi;

#ifdef WPINET_WEBSOCKET_VERBOSE_DEBUG
static std::string DebugBinary(std::span<const uint8_t> val) {
#ifdef WPINET_WEBSOCKET_VERBOSE_DEBUG_CONTENT
  std::string str;
  wpi::raw_string_ostream stros{str};
  for (auto ch : val) {
    stros << fmt::format("{:02x},", static_cast<unsigned int>(ch) & 0xff);
  }
  return str;
#else
  return "";
#endif
}

static inline std::string_view DebugText(std::string_view val) {
#ifdef WPINET_WEBSOCKET_VERBOSE_DEBUG_CONTENT
  return val;
#else
  return "";
#endif
}
#endif  // WPINET_WEBSOCKET_VERBOSE_DEBUG

class WebSocket::WriteReq : public uv::WriteReq,
                            public detail::WebSocketWriteReqBase {
 public:
  explicit WriteReq(
      std::weak_ptr<WebSocket> ws,
      std::function<void(std::span<uv::Buffer>, uv::Error)> callback)
      : m_ws{std::move(ws)}, m_callback{std::move(callback)} {
    finish.connect([this](uv::Error err) {
      int result = Continue(GetStream(), shared_from_this());
      WS_DEBUG("Continue() -> {}\n", result);
      if (result <= 0) {
        m_frames.ReleaseBufs();
        auto ws = m_ws.lock();
        if (ws) {
          ws->m_writeInProgress = false;
        }
        m_callback(m_userBufs, err);
        if (result == 0 && m_cont && ws) {
          WS_DEBUG("Continuing with another write\n");
          ws->m_stream.Write(m_cont->m_frames.m_bufs, m_cont);
        }
      }
    });
  }

  std::weak_ptr<WebSocket> m_ws;
  std::function<void(std::span<uv::Buffer>, uv::Error)> m_callback;
  std::shared_ptr<WriteReq> m_cont;
};

static constexpr uint8_t kFlagMasking = 0x80;
static constexpr uint8_t kLenMask = 0x7f;
static constexpr size_t kWriteAllocSize = 4096;

class WebSocket::ClientHandshakeData {
 public:
  ClientHandshakeData() {
    // key is a random nonce
    static std::random_device rd;
    static std::default_random_engine gen{rd()};
    std::uniform_int_distribution<unsigned int> dist(0, 255);
    char nonce[16];  // the nonce sent to the server
    for (char& v : nonce) {
      v = static_cast<char>(dist(gen));
    }
    raw_svector_ostream os(key);
    Base64Encode(os, {nonce, 16});
  }
  ~ClientHandshakeData() {
    if (auto t = timer.lock()) {
      t->Stop();
      t->Close();
    }
  }

  SmallString<64> key;                       // the key sent to the server
  SmallVector<std::string, 2> protocols;     // valid protocols
  HttpParser parser{HttpParser::kResponse};  // server response parser
  bool hasUpgrade = false;
  bool hasConnection = false;
  bool hasAccept = false;
  bool hasProtocol = false;

  std::weak_ptr<uv::Timer> timer;
};

static std::string_view AcceptHash(std::string_view key,
                                   SmallVectorImpl<char>& buf) {
  SHA1 hash;
  hash.Update(key);
  hash.Update("258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
  SmallString<64> hashBuf;
  return Base64Encode(hash.RawFinal(hashBuf), buf);
}

WebSocket::WebSocket(uv::Stream& stream, bool server, const private_init&)
    : m_stream{stream}, m_server{server} {
  // Connect closed and error signals to ourselves
  m_stream.closed.connect([this]() { SetClosed(1006, "handle closed"); });
  m_stream.error.connect([this](uv::Error err) {
    Terminate(1006, fmt::format("stream error: {}", err.name()));
  });

  // Start reading
  m_stream.StopRead();  // we may have been reading
  m_stream.StartRead();
  m_stream.data.connect(
      [this](uv::Buffer& buf, size_t size) { HandleIncoming(buf, size); });
  m_stream.end.connect(
      [this]() { Terminate(1006, "remote end closed connection"); });
}

WebSocket::~WebSocket() = default;

std::shared_ptr<WebSocket> WebSocket::CreateClient(
    uv::Stream& stream, std::string_view uri, std::string_view host,
    std::span<const std::string_view> protocols, const ClientOptions& options) {
  auto ws = std::make_shared<WebSocket>(stream, false, private_init{});
  stream.SetData(ws);
  ws->StartClient(uri, host, protocols, options);
  return ws;
}

std::shared_ptr<WebSocket> WebSocket::CreateServer(uv::Stream& stream,
                                                   std::string_view key,
                                                   std::string_view version,
                                                   std::string_view protocol) {
  auto ws = std::make_shared<WebSocket>(stream, true, private_init{});
  stream.SetData(ws);
  ws->StartServer(key, version, protocol);
  return ws;
}

void WebSocket::Close(uint16_t code, std::string_view reason) {
  SendClose(code, reason);
  if (m_state != FAILED && m_state != CLOSED) {
    m_state = CLOSING;
  }
}

void WebSocket::Fail(uint16_t code, std::string_view reason) {
  if (m_state == FAILED || m_state == CLOSED) {
    return;
  }
  SendClose(code, reason);
  SetClosed(code, reason, true);
  Shutdown();
}

void WebSocket::Terminate(uint16_t code, std::string_view reason) {
  if (m_state == FAILED || m_state == CLOSED) {
    return;
  }
  SetClosed(code, reason);
  Shutdown();
}

void WebSocket::StartClient(std::string_view uri, std::string_view host,
                            std::span<const std::string_view> protocols,
                            const ClientOptions& options) {
  // Create client handshake data
  m_clientHandshake = std::make_unique<ClientHandshakeData>();

  // Build client request
  SmallVector<uv::Buffer, 4> bufs;
  raw_uv_ostream os{bufs, kWriteAllocSize};

  os << "GET " << uri << " HTTP/1.1\r\n";
  os << "Host: " << host << "\r\n";
  os << "Upgrade: websocket\r\n";
  os << "Connection: Upgrade\r\n";
  os << "Sec-WebSocket-Key: " << m_clientHandshake->key << "\r\n";
  os << "Sec-WebSocket-Version: 13\r\n";

  // protocols (if provided)
  if (!protocols.empty()) {
    os << "Sec-WebSocket-Protocol: ";
    bool first = true;
    for (auto protocol : protocols) {
      if (!first) {
        os << ", ";
      } else {
        first = false;
      }
      os << protocol;
      // also save for later checking against server response
      m_clientHandshake->protocols.emplace_back(protocol);
    }
    os << "\r\n";
  }

  // other headers
  for (auto&& header : options.extraHeaders) {
    os << header.first << ": " << header.second << "\r\n";
  }

  // finish headers
  os << "\r\n";

  // Send client request
  m_stream.Write(bufs, [](auto bufs, uv::Error) {
    for (auto& buf : bufs) {
      buf.Deallocate();
    }
  });

  // Set up client response handling
  m_clientHandshake->parser.status.connect([this](std::string_view status) {
    unsigned int code = m_clientHandshake->parser.GetStatusCode();
    if (code != 101) {
      Terminate(code, status);
    }
  });
  m_clientHandshake->parser.header.connect(
      [this](std::string_view name, std::string_view value) {
        value = trim(value);
        if (equals_lower(name, "upgrade")) {
          if (!equals_lower(value, "websocket")) {
            return Terminate(1002, "invalid upgrade response value");
          }
          m_clientHandshake->hasUpgrade = true;
        } else if (equals_lower(name, "connection")) {
          if (!equals_lower(value, "upgrade")) {
            return Terminate(1002, "invalid connection response value");
          }
          m_clientHandshake->hasConnection = true;
        } else if (equals_lower(name, "sec-websocket-accept")) {
          // Check against expected response
          SmallString<64> acceptBuf;
          if (!equals(value, AcceptHash(m_clientHandshake->key, acceptBuf))) {
            return Terminate(1002, "invalid accept key");
          }
          m_clientHandshake->hasAccept = true;
        } else if (equals_lower(name, "sec-websocket-extensions")) {
          // No extensions are supported
          if (!value.empty()) {
            return Terminate(1010, "unsupported extension");
          }
        } else if (equals_lower(name, "sec-websocket-protocol")) {
          // Make sure it was one of the provided protocols
          bool match = false;
          for (auto&& protocol : m_clientHandshake->protocols) {
            if (equals_lower(value, protocol)) {
              match = true;
              break;
            }
          }
          if (!match) {
            return Terminate(1003, "unsupported protocol");
          }
          m_clientHandshake->hasProtocol = true;
          m_protocol = value;
        }
      });
  m_clientHandshake->parser.headersComplete.connect([this](bool) {
    if (!m_clientHandshake->hasUpgrade || !m_clientHandshake->hasConnection ||
        !m_clientHandshake->hasAccept ||
        (!m_clientHandshake->hasProtocol &&
         !m_clientHandshake->protocols.empty())) {
      return Terminate(1002, "invalid response");
    }
    if (m_state == CONNECTING) {
      m_state = OPEN;
      open(m_protocol);
    }
  });

  // Start handshake timer if a timeout was specified
  if (options.handshakeTimeout != (uv::Timer::Time::max)()) {
    if (auto timer = uv::Timer::Create(m_stream.GetLoopRef())) {
      timer->timeout.connect(
          [this]() { Terminate(1006, "connection timed out"); });
      timer->Start(options.handshakeTimeout);
      m_clientHandshake->timer = timer;
    }
  }
}

void WebSocket::StartServer(std::string_view key, std::string_view version,
                            std::string_view protocol) {
  m_protocol = protocol;

  // Build server response
  SmallVector<uv::Buffer, 4> bufs;
  raw_uv_ostream os{bufs, kWriteAllocSize};

  // Handle unsupported version
  if (version != "13") {
    os << "HTTP/1.1 426 Upgrade Required\r\n";
    os << "Upgrade: WebSocket\r\n";
    os << "Sec-WebSocket-Version: 13\r\n\r\n";
    m_stream.Write(bufs, [this](auto bufs, uv::Error) {
      for (auto& buf : bufs) {
        buf.Deallocate();
      }
      // XXX: Should we support sending a new handshake on the same connection?
      // XXX: "this->" is required by GCC 5.5 (bug)
      this->Terminate(1003, "unsupported protocol version");
    });
    return;
  }

  os << "HTTP/1.1 101 Switching Protocols\r\n";
  os << "Upgrade: websocket\r\n";
  os << "Connection: Upgrade\r\n";

  // accept hash
  SmallString<64> acceptBuf;
  os << "Sec-WebSocket-Accept: " << AcceptHash(key, acceptBuf) << "\r\n";

  if (!protocol.empty()) {
    os << "Sec-WebSocket-Protocol: " << protocol << "\r\n";
  }

  // end headers
  os << "\r\n";

  // Send server response
  m_stream.Write(bufs, [this](auto bufs, uv::Error) {
    for (auto& buf : bufs) {
      buf.Deallocate();
    }
    if (m_state == CONNECTING) {
      m_state = OPEN;
      open(m_protocol);
    }
  });
}

void WebSocket::SendClose(uint16_t code, std::string_view reason) {
  SmallVector<uv::Buffer, 4> bufs;
  if (code != 1005) {
    raw_uv_ostream os{bufs, kWriteAllocSize};
    const uint8_t codeMsb[] = {static_cast<uint8_t>((code >> 8) & 0xff),
                               static_cast<uint8_t>(code & 0xff)};
    os << std::span{codeMsb};
    os << reason;
  }
  SendControl(kFlagFin | kOpClose, bufs, [](auto bufs, uv::Error) {
    for (auto&& buf : bufs) {
      buf.Deallocate();
    }
  });
}

void WebSocket::SetClosed(uint16_t code, std::string_view reason, bool failed) {
  if (m_state == FAILED || m_state == CLOSED) {
    return;
  }
  m_state = failed ? FAILED : CLOSED;
  closed(code, reason);
}

void WebSocket::Shutdown() {
  m_stream.Shutdown([this] { m_stream.Close(); });
}

static inline void Unmask(std::span<uint8_t> data,
                          std::span<const uint8_t, 4> key) {
  int n = 0;
  for (uint8_t& ch : data) {
    ch ^= key[n++];
    if (n >= 4) {
      n = 0;
    }
  }
}

void WebSocket::HandleIncoming(uv::Buffer& buf, size_t size) {
  // ignore incoming data if we're failed or closed
  if (m_state == FAILED || m_state == CLOSED) {
    return;
  }

  std::string_view data{buf.base, size};

  // Handle connecting state (mainly on client)
  if (m_state == CONNECTING) {
    if (m_clientHandshake) {
      data = m_clientHandshake->parser.Execute(data);
      // check for parser failure
      if (m_clientHandshake->parser.HasError()) {
        return Terminate(1003, "invalid response");
      }
      if (m_state != OPEN) {
        return;  // not done with handshake yet
      }

      // we're done with the handshake, so release its memory
      m_clientHandshake.reset();

      // fall through to process additional data after handshake
    } else {
      return Terminate(1003, "got data on server before response");
    }
  }

  // Message processing
  while (!data.empty()) {
    if (m_frameSize == UINT64_MAX) {
      // Need at least two bytes to determine header length
      if (m_header.size() < 2u) {
        size_t toCopy = (std::min)(2u - m_header.size(), data.size());
        m_header.append(data.data(), data.data() + toCopy);
        data.remove_prefix(toCopy);
        if (m_header.size() < 2u) {
          return;  // need more data
        }

        // Validate RSV bits are zero
        if ((m_header[0] & 0x70) != 0) {
          return Fail(1002, "nonzero RSV");
        }
      }

      // Once we have first two bytes, we can calculate the header size
      if (m_headerSize == 0) {
        m_headerSize = 2;
        uint8_t len = m_header[1] & kLenMask;
        if (len == 126) {
          m_headerSize += 2;
        } else if (len == 127) {
          m_headerSize += 8;
        }
        bool masking = (m_header[1] & kFlagMasking) != 0;
        if (masking) {
          m_headerSize += 4;  // masking key
        }
        // On server side, incoming messages MUST be masked
        // On client side, incoming messages MUST NOT be masked
        if (m_server && !masking) {
          return Fail(1002, "client data not masked");
        }
        if (!m_server && masking) {
          return Fail(1002, "server data masked");
        }
      }

      // Need to complete header to calculate message size
      if (m_header.size() < m_headerSize) {
        size_t toCopy = (std::min)(m_headerSize - m_header.size(), data.size());
        m_header.append(data.data(), data.data() + toCopy);
        data.remove_prefix(toCopy);
        if (m_header.size() < m_headerSize) {
          return;  // need more data
        }
      }

      if (m_header.size() >= m_headerSize) {
        // get payload length
        uint8_t len = m_header[1] & kLenMask;
        if (len == 126) {
          m_frameSize = (static_cast<uint16_t>(m_header[2]) << 8) |
                        static_cast<uint16_t>(m_header[3]);
        } else if (len == 127) {
          m_frameSize = (static_cast<uint64_t>(m_header[2]) << 56) |
                        (static_cast<uint64_t>(m_header[3]) << 48) |
                        (static_cast<uint64_t>(m_header[4]) << 40) |
                        (static_cast<uint64_t>(m_header[5]) << 32) |
                        (static_cast<uint64_t>(m_header[6]) << 24) |
                        (static_cast<uint64_t>(m_header[7]) << 16) |
                        (static_cast<uint64_t>(m_header[8]) << 8) |
                        static_cast<uint64_t>(m_header[9]);
        } else {
          m_frameSize = len;
        }

        // limit maximum size
        bool control = (m_header[0] & kFlagControl) != 0;
        if (((control ? m_controlPayload.size() : m_payload.size()) +
             m_frameSize) > m_maxMessageSize) {
          return Fail(1009, "message too large");
        }
      }
    }

    if (m_frameSize != UINT64_MAX) {
      bool control = (m_header[0] & kFlagControl) != 0;
      size_t need;
      if (control) {
        need = m_frameSize - m_controlPayload.size();
      } else {
        need = m_frameStart + m_frameSize - m_payload.size();
      }
      size_t toCopy = (std::min)(need, data.size());
      if (control) {
        m_controlPayload.append(data.data(), data.data() + toCopy);
      } else {
        m_payload.append(data.data(), data.data() + toCopy);
      }
      data.remove_prefix(toCopy);
      need -= toCopy;
      if (need == 0) {
        // We have a complete frame
        // If the message had masking, unmask it
        if ((m_header[1] & kFlagMasking) != 0) {
          Unmask(control ? std::span{m_controlPayload}
                         : std::span{m_payload}.subspan(m_frameStart),
                 std::span<const uint8_t, 4>{&m_header[m_headerSize - 4], 4});
        }

        // Handle message
        bool fin = (m_header[0] & kFlagFin) != 0;
        uint8_t opcode = m_header[0] & kOpMask;
        switch (opcode) {
          case kOpCont:
            WS_DEBUG("WS Fragment {} [{}]\n", m_payload.size(),
                     DebugBinary(m_payload));
            switch (m_fragmentOpcode) {
              case kOpText:
                if (!m_combineFragments || fin) {
                  std::string_view content{
                      reinterpret_cast<char*>(m_payload.data()),
                      m_payload.size()};
                  WS_DEBUG("WS RecvText(Defrag) {} ({})\n", m_payload.size(),
                           DebugText(content));
                  text(content, fin);
                }
                break;
              case kOpBinary:
                if (!m_combineFragments || fin) {
                  WS_DEBUG("WS RecvBinary(Defrag) {} ({})\n", m_payload.size(),
                           DebugBinary(m_payload));
                  binary(m_payload, fin);
                }
                break;
              default:
                // no preceding message?
                return Fail(1002, "invalid continuation message");
            }
            if (fin) {
              m_fragmentOpcode = 0;
            }
            break;
          case kOpText: {
            std::string_view content{reinterpret_cast<char*>(m_payload.data()),
                                     m_payload.size()};
            if (m_fragmentOpcode != 0) {
              WS_DEBUG("WS RecvText {} ({}) -> INCOMPLETE FRAGMENT\n",
                       m_payload.size(), DebugText(content));
              return Fail(1002, "incomplete fragment");
            }
            if (!m_combineFragments || fin) {
              WS_DEBUG("WS RecvText {} ({})\n", m_payload.size(),
                       DebugText(content));
              text(content, fin);
            }
            if (!fin) {
              WS_DEBUG("WS RecvText {} StartFrag\n", m_payload.size());
              m_fragmentOpcode = opcode;
            }
            break;
          }
          case kOpBinary:
            if (m_fragmentOpcode != 0) {
              WS_DEBUG("WS RecvBinary {} ({}) -> INCOMPLETE FRAGMENT\n",
                       m_payload.size(), DebugBinary(m_payload));
              return Fail(1002, "incomplete fragment");
            }
            if (!m_combineFragments || fin) {
              WS_DEBUG("WS RecvBinary {} ({})\n", m_payload.size(),
                       DebugBinary(m_payload));
              binary(m_payload, fin);
            }
            if (!fin) {
              WS_DEBUG("WS RecvBinary {} StartFrag\n", m_payload.size());
              m_fragmentOpcode = opcode;
            }
            break;
          case kOpClose: {
            uint16_t code;
            std::string_view reason;
            if (!fin) {
              code = 1002;
              reason = "cannot fragment control frames";
            } else if (m_controlPayload.size() < 2) {
              code = 1005;
            } else {
              code = (static_cast<uint16_t>(m_controlPayload[0]) << 8) |
                     static_cast<uint16_t>(m_controlPayload[1]);
              reason =
                  drop_front({reinterpret_cast<char*>(m_controlPayload.data()),
                              m_controlPayload.size()},
                             2);
            }
            // Echo the close if we didn't previously send it
            if (m_state != CLOSING) {
              SendClose(code, reason);
            }
            SetClosed(code, reason);
            // If we're the server, shutdown the connection.
            if (m_server) {
              Shutdown();
            }
            break;
          }
          case kOpPing:
            if (!fin) {
              return Fail(1002, "cannot fragment control frames");
            }
            // If the connection is open, send a Pong in response
            if (m_state == OPEN) {
              SmallVector<uv::Buffer, 4> bufs;
              {
                raw_uv_ostream os{bufs, kWriteAllocSize};
                os << m_controlPayload;
              }
              SendPong(bufs, [](auto bufs, uv::Error) {
                for (auto&& buf : bufs) {
                  buf.Deallocate();
                }
              });
            }
            WS_DEBUG("WS RecvPing() {} ({})\n", m_controlPayload.size(),
                     DebugBinary(m_controlPayload));
            ping(m_controlPayload);
            break;
          case kOpPong:
            if (!fin) {
              return Fail(1002, "cannot fragment control frames");
            }
            WS_DEBUG("WS RecvPong() {} ({})\n", m_controlPayload.size(),
                     DebugBinary(m_controlPayload));
            pong(m_controlPayload);
            break;
          default:
            return Fail(1002, "invalid message opcode");
        }

        // Prepare for next message
        m_header.clear();
        m_headerSize = 0;
        if (!m_combineFragments || fin) {
          if (control) {
            m_controlPayload.clear();
          } else {
            m_payload.clear();
          }
        }
        m_frameStart = m_payload.size();
        m_frameSize = UINT64_MAX;
      }
    }
  }
}

static void VerboseDebug(const WebSocket::Frame& frame) {
#ifdef WPINET_WEBSOCKET_VERBOSE_DEBUG
  if ((frame.opcode & 0x7f) == 0x01) {
    SmallString<128> str;
#ifdef WPINET_WEBSOCKET_VERBOSE_DEBUG_CONTENT
    for (auto&& d : frame.data) {
      str.append(std::string_view(d.base, d.len));
    }
#endif
    fmt::print("WS SendText({})\n", str.str());
  } else if ((frame.opcode & 0x7f) == 0x02) {
    SmallString<128> str;
#ifdef WPINET_WEBSOCKET_VERBOSE_DEBUG_CONTENT
    raw_svector_ostream stros{str};
    for (auto&& d : frame.data) {
      for (auto ch : d.data()) {
        stros << fmt::format("{:02x},", static_cast<unsigned int>(ch) & 0xff);
      }
    }
#endif
    fmt::print("WS SendBinary({})\n", str.str());
  } else {
    SmallString<128> str;
#ifdef WPINET_WEBSOCKET_VERBOSE_DEBUG_CONTENT
    raw_svector_ostream stros{str};
    for (auto&& d : frame.data) {
      for (auto ch : d.data()) {
        stros << fmt::format("{:02x},", static_cast<unsigned int>(ch) & 0xff);
      }
    }
#endif
    fmt::print("WS SendOp({}, {})\n", frame.opcode, str.str());
  }
#endif
}

void WebSocket::SendFrames(
    std::span<const Frame> frames,
    std::function<void(std::span<uv::Buffer>, uv::Error)> callback) {
  // If we're not open, emit an error and don't send the data
  WS_DEBUG("SendFrames({})\n", frames.size());
  if (m_state != OPEN) {
    SendError(frames, callback);
    return;
  }

  auto req = std::make_shared<WriteReq>(std::weak_ptr<WebSocket>{},
                                        std::move(callback));
  for (auto&& frame : frames) {
    VerboseDebug(frame);
    req->m_frames.AddFrame(frame, m_server);
    req->m_userBufs.append(frame.data.begin(), frame.data.end());
  }
  req->m_continueBufPos = req->m_frames.m_bufs.size();
  if (m_writeInProgress) {
    if (auto curReq = m_writeReq.lock()) {
      // if write currently in progress, process as a continuation of that
      m_writeReq = req;
      curReq->m_cont = std::move(req);
      return;
    }
  }
  WS_DEBUG("Write({})\n", req->m_frames.m_bufs.size());
  m_stream.Write(req->m_frames.m_bufs, req);
}

std::span<const WebSocket::Frame> WebSocket::TrySendFrames(
    std::span<const Frame> frames,
    std::function<void(std::span<uv::Buffer>, uv::Error)> callback) {
  // If we're not open, emit an error and don't send the data
  if (m_state != WebSocket::OPEN) {
    SendError(frames, callback);
    return {};
  }

  // If something else is still in flight, don't send anything
  if (m_writeInProgress) {
    return frames;
  }

  return detail::TrySendFrames(
      m_server, m_stream, frames,
      [this](std::function<void(std::span<uv::Buffer>, uv::Error)>&& cb) {
        auto req = std::make_shared<WriteReq>(weak_from_this(), std::move(cb));
        m_writeInProgress = true;
        m_writeReq = req;
        return req;
      },
      std::move(callback));
}

void WebSocket::SendControl(
    uint8_t opcode, std::span<const uv::Buffer> data,
    std::function<void(std::span<uv::Buffer>, uv::Error)> callback) {
  Frame frame{opcode, data};
  // If we're not open, emit an error and don't send the data
  if (m_state != WebSocket::OPEN) {
    SendError({{frame}}, callback);
    return;
  }

  // Control messages always send immediately with their own request, whether
  // or not something else is in flight. The protocol allows control messages
  // interspersed with fragmented frames, and we otherwise make sure that any
  // pending Write() is already at a frame boundary.
  auto req = std::make_shared<WriteReq>(std::weak_ptr<WebSocket>{},
                                        std::move(callback));
  VerboseDebug(frame);
  req->m_frames.AddFrame(frame, m_server);
  req->m_userBufs.append(frame.data.begin(), frame.data.end());
  req->m_continueBufPos = req->m_frames.m_bufs.size();
  m_stream.Write(req->m_frames.m_bufs, req);
}

void WebSocket::SendError(
    std::span<const Frame> frames,
    const std::function<void(std::span<uv::Buffer>, uv::Error)>& callback) {
  int err;
  if (m_state == WebSocket::CONNECTING) {
    err = UV_EAGAIN;
  } else {
    err = UV_ESHUTDOWN;
  }
  SmallVector<uv::Buffer, 4> bufs;
  for (auto&& frame : frames) {
    bufs.append(frame.data.begin(), frame.data.end());
  }
  callback(bufs, uv::Error{err});
}
