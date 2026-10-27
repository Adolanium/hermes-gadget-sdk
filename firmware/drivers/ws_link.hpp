// The WebSocket transport's bookkeeping, apart from ESP-IDF so it can be tested
// on the host: which connection is current, whether it was lost, and what the
// send task does with each queued item (firmware/esp32/main/port_ws.cpp).
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace hg::ws {

// Each connection gets a generation. Leaving a connection moves to the next
// one, so whatever the old client still reports can be told apart and ignored.
class Generations {
 public:
  uint32_t current() const { return current_.load(); }
  bool is_current(uint32_t generation) const { return generation == current_.load(); }
  // The app leaves the current connection (before connecting again, or for good).
  void leave() { ++current_; }
  // A client reports its connection lost (any task).
  void lost(uint32_t generation) { lost_.store(generation); }
  // True once per loss of the current connection; losses of connections the app
  // has already left are not news.
  bool take_lost() {
    uint32_t g = lost_.exchange(0);
    return g && g == current_.load();
  }

 private:
  std::atomic<uint32_t> current_{0};
  std::atomic<uint32_t> lost_{0};
};

constexpr uint8_t kText = 0x1;
constexpr uint8_t kBinary = 0x2;
constexpr uint8_t kClose = 0xFF;  // not a WebSocket opcode: close and destroy the client

// The send queue holds messages plus a few slots only "close this client" may
// use, so leaving a connection never waits for a slow link to drain. Of the
// message slots, the last few are for text: when a slow uplink fills the queue
// with microphone audio, the frames are refused first, so a control message such
// as audio.end, which ends the recording, still gets through.
constexpr size_t kTxMessages = 64;
constexpr size_t kTxReserved = 4;
constexpr size_t kTxControl = 8;
inline bool may_queue_message(size_t free_slots, uint8_t opcode) {
  return free_slots > kTxReserved + (opcode == kBinary ? kTxControl : 0);
}

// Every network wait has a limit. The library's own esp_websocket_client_close()
// waits for its close frame without one and froze the app on a dead link.
constexpr uint32_t kSendTimeoutMs = 2000;
constexpr uint32_t kCloseTimeoutMs = 1000;
// The client's own reads and connects (esp_websocket_client_config_t::network_timeout_ms).
constexpr uint32_t kNetworkTimeoutMs = 10000;

// Handing a client to the send task for closing waits at most this long for a
// queue slot. The reserved slots normally make it immediate.
constexpr uint32_t kHandoffTimeoutMs = 1000;

// Each item has a limit: a send takes at most its lock wait and its write, and a
// close also waits for the client's task to end, which is bounded by
// kNetworkTimeoutMs. An item that takes longer than this has the send task stuck
// in the client library: nothing can be sent or closed again, and the device
// would answer nothing until someone reset it. Only a restart recovers.
constexpr uint32_t kStuckMs = 60000;

// Whether the send task is stuck on one item. The send task calls begin() and
// end() around each item; any task may ask stuck().
class TxWatch {
 public:
  void begin(uint32_t now_ms) {
    since_.store(now_ms);
    busy_.store(true);
  }
  void end() { busy_.store(false); }
  // Signed: `now_ms` is read before since_, so an item the send task began in between on the
  // other core is a few milliseconds in the future, not ~49 days old. Holds across the wrap.
  bool stuck(uint32_t now_ms) const {
    return busy_.load() && static_cast<int32_t>(now_ms - since_.load()) > static_cast<int32_t>(kStuckMs);
  }

 private:
  std::atomic<bool> busy_{false};
  std::atomic<uint32_t> since_{0};
};

struct TxItem {
  void* client;
  uint32_t generation;
  uint8_t opcode;  // kText, kBinary or kClose
  const uint8_t* data;
  size_t len;
};

enum class Outcome { Sent, Failed, Dropped, Destroyed };

// What the send task does with one item. Ops supplies the client calls:
//   bool is_connected(void* client)
//   int  send(void* client, uint8_t opcode, const uint8_t* data, size_t len, uint32_t timeout_ms)
//   void send_close(void* client, uint32_t timeout_ms)
//   void destroy(void* client)
// A failed send needs no handling here: the client aborts the connection and
// reports it, which ends in Generations::lost().
template <class Ops>
Outcome process(const TxItem& item, uint32_t current_generation, Ops& ops) {
  if (item.opcode == kClose) {
    if (ops.is_connected(item.client)) ops.send_close(item.client, kCloseTimeoutMs);
    ops.destroy(item.client);
    return Outcome::Destroyed;
  }
  if (item.generation != current_generation || !ops.is_connected(item.client)) return Outcome::Dropped;
  int sent = ops.send(item.client, item.opcode, item.data, item.len, kSendTimeoutMs);
  return sent == static_cast<int>(item.len) ? Outcome::Sent : Outcome::Failed;
}

}  // namespace hg::ws
