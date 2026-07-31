// The USB tunnel end to end, against a stub client -- the parts no unit test can reach: does the
// TLS handshake complete, is an unpaired cert actually refused, does NEED_DATA reach the client,
// and does a DATA connection with a bad token get dropped.
//
// Needs only loopback sockets and generated certs: no GPU, no kernel module, no root, no device.
//
// The rejection cases matter more than the acceptance case. Accepting a good client is visible the
// moment you try it; silently accepting a BAD one is the whole security argument failing quietly,
// and everything past that point feeds bytes to the host kernel's URB parser.

#include <atomic>
#include <cstdio>
#include <cstring>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <string>
#include <thread>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <crypto/crypto.hpp>
#include <server/usb_tunnel.hpp>
#include <server/usb_tunnel_proto.hpp>

using namespace usbip;

static int failures = 0;
#define CHECK(cond, what)                                                                                    \
  do {                                                                                                       \
    if (cond)                                                                                                \
      std::printf("  ok   %s\n", what);                                                                      \
    else {                                                                                                   \
      std::printf("  FAIL %s\n", what);                                                                      \
      failures++;                                                                                            \
    }                                                                                                        \
  } while (0)

namespace {

constexpr int kPort = 48211; // not the production port: never collide with a running server

struct Identity {
  x509::pkey_ptr key;
  x509::x509_ptr cert;
  std::string cert_path, key_path;
};

Identity make_identity(const std::string &stem) {
  Identity id;
  id.key = x509::generate_key();
  id.cert = x509::generate_x509(id.key);
  id.cert_path = "/tmp/" + stem + "-cert.pem";
  id.key_path = "/tmp/" + stem + "-key.pem";
  x509::write_to_disk(id.key, id.key_path, id.cert, id.cert_path);
  return id;
}

// A stub Moonlight client: TLS in, preamble, frames. This is the fork's logic in miniature, and
// writing it here is what de-risks writing it in Qt.
struct StubClient {
  SSL_CTX *ctx = nullptr;
  SSL *ssl = nullptr;
  int fd = -1;
  tunnel::FrameDecoder dec;

  bool connect(const Identity &id, tunnel::Channel ch, std::uint32_t token) {
    ctx = SSL_CTX_new(TLS_client_method());
    if (SSL_CTX_use_certificate_file(ctx, id.cert_path.c_str(), SSL_FILETYPE_PEM) != 1 ||
        SSL_CTX_use_PrivateKey_file(ctx, id.key_path.c_str(), SSL_FILETYPE_PEM) != 1)
      return false;

    fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(kPort);
    ::inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (::connect(fd, reinterpret_cast<sockaddr *>(&a), sizeof(a)) != 0)
      return false;

    timeval tv{3, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    ssl = SSL_new(ctx);
    SSL_set_fd(ssl, fd);
    if (SSL_connect(ssl) != 1)
      return false;

    tunnel::Preamble p;
    p.channel = ch;
    p.token = token;
    auto pre = tunnel::encode_preamble(p);
    return SSL_write(ssl, pre.data(), static_cast<int>(pre.size())) == static_cast<int>(pre.size());
  }

  bool send(tunnel::FrameType t, const std::string &payload = {}) {
    auto f = tunnel::encode_frame(t, payload);
    return SSL_write(ssl, f.data(), static_cast<int>(f.size())) == static_cast<int>(f.size());
  }

  // Waits for a frame of the given type. PINGs are answered rather than discarded -- a real
  // client must, and a stub that stays silent gets dropped after three misses, which would make
  // every test after this one fail for the wrong reason.
  bool wait_for(tunnel::FrameType want, tunnel::Frame &out, int tries = 40) {
    char buf[4096];
    std::vector<tunnel::Frame> frames;
    for (int i = 0; i < tries; i++) {
      for (auto &f : frames) {
        if (f.type == tunnel::FrameType::PING)
          send(tunnel::FrameType::PONG);
        if (f.type == want) {
          out = f;
          return true;
        }
      }
      frames.clear();
      int r = SSL_read(ssl, buf, sizeof(buf));
      if (r <= 0)
        return false;
      if (!dec.feed(buf, static_cast<std::size_t>(r), frames))
        return false;
      for (auto &f : frames) {
        if (f.type == tunnel::FrameType::PING)
          send(tunnel::FrameType::PONG);
        if (f.type == want) {
          out = f;
          return true;
        }
      }
      frames.clear();
    }
    return false;
  }

  void close() {
    if (ssl) {
      SSL_shutdown(ssl);
      SSL_free(ssl);
      ssl = nullptr;
    }
    if (fd >= 0) {
      ::close(fd);
      fd = -1;
    }
    if (ctx) {
      SSL_CTX_free(ctx);
      ctx = nullptr;
    }
  }
};

} // namespace

int main() {
  SSL_library_init();
  OpenSSL_add_all_algorithms();

  auto server_id = make_identity("sstun-server");
  auto paired = make_identity("sstun-paired");
  auto stranger = make_identity("sstun-stranger");

  // Stands in for AppState's paired-client store.
  auto paired_pem = x509::get_cert_pem(paired.cert);
  auto verify = [&](const x509::x509_ptr &cert) {
    return !x509::verification_error(x509::cert_from_string(paired_pem), cert).has_value();
  };

  TunnelServer server;
  std::printf("== listener ==\n");
  bool up = server.start(kPort, server_id.cert_path, server_id.key_path, verify);
  CHECK(up, "tunnel binds and starts accepting");
  if (!up) {
    std::printf("\nTUNNEL TESTS FAILED (could not listen)\n");
    return 1;
  }
  CHECK(server.running(), "running() reports true once bound");
  CHECK(!server.has_client(), "no client before anyone connects");
  CHECK(server.transport() == nullptr, "no transport without a client");

  std::printf("\n== an UNPAIRED client is refused ==\n");
  {
    StubClient bad;
    bool connected = bad.connect(stranger, tunnel::Channel::CONTROL, 0);
    // TLS itself may well complete -- the server accepts any cert at the TLS layer by design and
    // checks the paired store afterwards. What must NOT happen is the client becoming usable.
    bad.send(tunnel::FrameType::HELLO, tunnel::encode_hello(1, "stranger"));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(!server.has_client(), "an unpaired cert never becomes the control client");
    CHECK(server.transport() == nullptr, "and yields no transport, so nothing can be imported");
    (void)connected;
    bad.close();
  }

  std::printf("\n== a PAIRED client is accepted ==\n");
  StubClient good;
  {
    CHECK(good.connect(paired, tunnel::Channel::CONTROL, 0), "paired client completes TLS + preamble");
    good.send(tunnel::FrameType::HELLO, tunnel::encode_hello(42, "htpc"));
    good.send(tunnel::FrameType::OFFER, tunnel::encode_offer({{"1-4", 0x046d, 0xc262, "G920"},
                                                              {"2-1", 0x28de, 0x1304, "Puck"}}));
    bool seen = false;
    for (int i = 0; i < 50 && !seen; i++) {
      seen = server.has_client() && server.offered().size() == 2;
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(seen, "HELLO + OFFER reach the server");
    CHECK(server.client_session() == 42, "the session id from HELLO is recorded");
    auto offers = server.offered();
    CHECK(offers.size() == 2 && offers[0].busid == "1-4" && offers[0].vid == 0x046d,
          "the offered device list survives the wire");
  }

  std::printf("\n== the server can initiate an import ==\n");
  {
    auto tr = server.transport();
    CHECK(tr != nullptr, "a connected client yields a transport");
    CHECK(tr && tr->busids().size() == 2, "the transport imports exactly what was offered");

    // open() blocks waiting for the client's DATA connection, so drive the client concurrently.
    std::atomic<bool> got_need{false};
    std::string need_busid;
    std::uint32_t need_token = 0;
    std::thread client([&] {
      tunnel::Frame f;
      if (good.wait_for(tunnel::FrameType::NEED_DATA, f))
        got_need = tunnel::decode_need_data(f.payload, need_token, need_busid);
      if (got_need) {
        // What the real client does here is splice to its local usbipd. We only need to prove the
        // connection is accepted and handed to the waiting importer.
        StubClient data;
        data.connect(paired, tunnel::Channel::DATA, need_token);
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        data.close();
      }
    });

    auto ch = tr ? tr->open("1-4", 3000) : nullptr;
    client.join();

    CHECK(got_need.load(), "the client receives NEED_DATA over the control channel");
    CHECK(need_busid == "1-4", "NEED_DATA names the device the server wants");
    CHECK(need_token != 0, "NEED_DATA carries a non-zero token");
    CHECK(ch != nullptr, "the DATA connection is handed to the waiting importer as a Channel");
  }

  std::printf("\n== a DATA connection with a bogus token is dropped ==\n");
  {
    // The token is the authorization for the second connection. A paired client is not, by itself,
    // entitled to be spliced to an arbitrary device.
    StubClient rogue;
    bool c = rogue.connect(paired, tunnel::Channel::DATA, 0xAAAAAAAA);
    CHECK(c, "the rogue DATA connection completes TLS (it has a paired cert)");
    char b[1];
    // The server closes it; a read must not succeed.
    int r = c ? SSL_read(rogue.ssl, b, 1) : -1;
    CHECK(r <= 0, "but the server closes it rather than splicing it to a device");
    rogue.close();
  }

  std::printf("\n== timeout when the client never opens DATA ==\n");
  {
    auto tr = server.transport();
    auto t0 = std::chrono::steady_clock::now();
    auto ch = tr ? tr->open("2-1", 400) : nullptr;
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    CHECK(ch == nullptr, "open() gives up rather than blocking the stream forever");
    CHECK(ms >= 350 && ms < 2000, "it waits about as long as asked, then returns");
  }

  std::printf("\n== client disconnect ==\n");
  {
    good.close();
    bool gone = false;
    for (int i = 0; i < 100 && !gone; i++) {
      gone = !server.has_client();
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(gone, "the server notices the client went away");
    CHECK(server.offered().empty(), "and forgets what it had offered");
    CHECK(server.transport() == nullptr, "so a later session cannot import through a dead link");
  }

  server.stop();
  CHECK(!server.running(), "stop() shuts the listener down");

  for (auto *p : {&server_id, &paired, &stranger}) {
    ::unlink(p->cert_path.c_str());
    ::unlink(p->key_path.c_str());
  }

  if (failures) {
    std::printf("\n%d TUNNEL TEST(S) FAILED\n", failures);
    return 1;
  }
  std::printf("\nALL TUNNEL TESTS PASSED\n");
  return 0;
}
