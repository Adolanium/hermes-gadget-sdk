#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "check.hpp"
#include "ws_link.hpp"

namespace {

using hg::ws::Generations;
using hg::ws::Outcome;
using hg::ws::TxItem;

// Records the client calls hg::ws::process() makes.
struct FakeOps {
  std::vector<std::string> calls;
  std::vector<void*> connected;
  int short_by = 0;  // a send that writes this many bytes too few

  static std::string name(void* c) { return std::string(1, *static_cast<char*>(c)); }
  bool is_connected(void* c) {
    for (void* x : connected)
      if (x == c) return true;
    return false;
  }
  int send(void* c, uint8_t opcode, const uint8_t*, size_t len, uint32_t timeout_ms) {
    calls.push_back(std::string(opcode == hg::ws::kBinary ? "binary " : "text ") + name(c) + " " +
                    std::to_string(len) + " " + std::to_string(timeout_ms) + "ms");
    return static_cast<int>(len) - short_by;
  }
  void send_close(void* c, uint32_t timeout_ms) {
    calls.push_back("close frame " + name(c) + " " + std::to_string(timeout_ms) + "ms");
  }
  void destroy(void* c) { calls.push_back("destroy " + name(c)); }
};

char client_a = 'A', client_b = 'B';
const uint8_t kHello[] = {'h', 'e', 'l', 'l', 'o'};

TxItem message(void* client, uint32_t generation, uint8_t opcode = hg::ws::kText) {
  return TxItem{client, generation, opcode, kHello, sizeof(kHello)};
}
TxItem close_item(void* client) { return TxItem{client, 0, hg::ws::kClose, nullptr, 0}; }

}  // namespace

TEST("ws: blocked receive admission owns its frame while a replacement client reuses the buffer") {
  std::string reassembly(4096, 'a');
  reassembly[0] = '\x03';
  const std::string original = reassembly;
  const bool accepted = hg::ws::deliver_owned_message(reassembly, [&](const uint8_t* data, size_t len) {
    // Simulate a new client's callback running while OTA admission is blocked.
    // Force reallocation, then leave a partial next frame for its later callback.
    reassembly.assign(65536, 'b');
    CHECK_EQ(std::string(reinterpret_cast<const char*>(data), len), original);
    reassembly = "replacement partial frame";
    return true;
  });
  CHECK(accepted);
  CHECK_EQ(reassembly, "replacement partial frame");
}

TEST("ws: refused receive admission preserves replacement bytes and binary short frames") {
  std::string reassembly("\x03\0\x01\0old", 7);
  const std::string original = reassembly;
  CHECK(!hg::ws::deliver_owned_message(reassembly, [&](const uint8_t* data, size_t len) {
    reassembly = "new";
    CHECK_EQ(std::string(reinterpret_cast<const char*>(data), len), original);
    return false;
  }));
  CHECK_EQ(reassembly, "new");
}

// --- Which connection is current, and whether it was lost -------------------

TEST("ws: nothing is lost before the first connection") {
  Generations g;
  CHECK(!g.take_lost());
}

TEST("ws: events from a connection the app has left are ignored") {
  Generations g;
  g.leave();
  const uint32_t first = g.current();
  g.leave();
  CHECK(!g.is_current(first));
  CHECK(g.is_current(g.current()));
}

TEST("ws: a lost connection is reported once") {
  Generations g;
  g.leave();
  g.lost(g.current());
  CHECK(g.take_lost());
  CHECK(!g.take_lost());
}

TEST("ws: a loss the app already acted on is not reported again") {
  Generations g;
  g.leave();
  g.lost(g.current());
  g.leave();  // the app closed that connection before the main loop looked
  CHECK(!g.take_lost());
}

TEST("ws: the old client's loss doesn't end the new connection") {
  Generations g;
  g.leave();
  const uint32_t old_conn = g.current();
  g.leave();  // reconnecting: the old client is torn down in the background
  g.lost(old_conn);
  CHECK(!g.take_lost());
  g.lost(g.current());
  CHECK(g.take_lost());
}

// --- The send queue ----------------------------------------------------------

TEST("ws: messages leave the reserved slots free") {
  CHECK(hg::ws::may_queue_message(hg::ws::kTxReserved + 1, hg::ws::kText));
  CHECK(!hg::ws::may_queue_message(hg::ws::kTxReserved, hg::ws::kText));
  CHECK(!hg::ws::may_queue_message(0, hg::ws::kText));
}

TEST("ws: a queue full of audio still takes the messages that end a recording, and a close") {
  size_t free_slots = hg::ws::kTxMessages + hg::ws::kTxReserved, frames = 0, texts = 0;
  while (hg::ws::may_queue_message(free_slots, hg::ws::kBinary)) {
    --free_slots;
    ++frames;
  }
  CHECK_EQ(frames, hg::ws::kTxMessages - hg::ws::kTxControl);
  while (hg::ws::may_queue_message(free_slots, hg::ws::kText)) {
    --free_slots;
    ++texts;
  }
  CHECK_EQ(texts, hg::ws::kTxControl);
  CHECK(free_slots >= 1u);  // close() waits for a slot; this one is always there
}

TEST("ws: a message for the current connection is sent with a time limit") {
  FakeOps ops;
  ops.connected = {&client_a};
  CHECK(hg::ws::process(message(&client_a, 1), 1, ops) == Outcome::Sent);
  CHECK(hg::ws::process(message(&client_a, 1, hg::ws::kBinary), 1, ops) == Outcome::Sent);
  CHECK_EQ(ops.calls.size(), 2u);
  CHECK_EQ(ops.calls[0], std::string("text A 5 10000ms"));
  CHECK_EQ(ops.calls[1], std::string("binary A 5 10000ms"));
}

TEST("ws: a short send is reported as failed") {
  FakeOps ops;
  ops.connected = {&client_a};
  ops.short_by = 2;
  CHECK(hg::ws::process(message(&client_a, 1), 1, ops) == Outcome::Failed);
}

TEST("ws: messages for a connection the app has left are dropped unsent") {
  FakeOps ops;
  ops.connected = {&client_a};
  CHECK(hg::ws::process(message(&client_a, 1), 2, ops) == Outcome::Dropped);
  CHECK(ops.calls.empty());
}

TEST("ws: messages for a client that lost its link are dropped unsent") {
  FakeOps ops;  // client A is no longer connected
  CHECK(hg::ws::process(message(&client_a, 1), 1, ops) == Outcome::Dropped);
  CHECK(ops.calls.empty());
}

TEST("ws: the old client is closed after its queued messages, and the new one carries on") {
  FakeOps ops;
  ops.connected = {&client_a, &client_b};
  Generations g;
  g.leave();
  std::deque<TxItem> queue;
  queue.push_back(message(&client_a, g.current()));
  g.leave();  // the app reconnects: close(A), then messages for B
  queue.push_back(close_item(&client_a));
  queue.push_back(message(&client_b, g.current()));
  std::vector<Outcome> outcomes;
  for (const TxItem& item : queue) outcomes.push_back(hg::ws::process(item, g.current(), ops));
  CHECK(outcomes[0] == Outcome::Dropped);  // A's message is stale by the time it is sent
  CHECK(outcomes[1] == Outcome::Destroyed);
  CHECK(outcomes[2] == Outcome::Sent);
  CHECK_EQ(ops.calls.size(), 3u);
  CHECK_EQ(ops.calls[0], std::string("close frame A 1000ms"));
  CHECK_EQ(ops.calls[1], std::string("destroy A"));
  CHECK_EQ(ops.calls[2], std::string("text B 5 10000ms"));
}

// --- Closing never waits without a limit (the Wi-Fi-loss freeze) -------------

TEST("ws: closing sends the close frame with a time limit, then destroys the client") {
  FakeOps ops;
  ops.connected = {&client_a};
  CHECK(hg::ws::process(close_item(&client_a), 7, ops) == Outcome::Destroyed);
  CHECK_EQ(ops.calls.size(), 2u);
  CHECK_EQ(ops.calls[0], std::string("close frame A 1000ms"));
  CHECK_EQ(ops.calls[1], std::string("destroy A"));
}

TEST("ws: closing a client whose link is gone skips the close frame") {
  FakeOps ops;  // Wi-Fi lost: nothing could be written anyway
  CHECK(hg::ws::process(close_item(&client_a), 7, ops) == Outcome::Destroyed);
  CHECK_EQ(ops.calls.size(), 1u);
  CHECK_EQ(ops.calls[0], std::string("destroy A"));
}

TEST("ws: every network wait has a limit of a few seconds at most") {
  CHECK(hg::ws::kSendTimeoutMs > 0 && hg::ws::kSendTimeoutMs <= 15000);
  CHECK(hg::ws::kCloseTimeoutMs > 0 && hg::ws::kCloseTimeoutMs <= 5000);
}

TEST("ws: a send rides out a TCP stall of several seconds") {
  // A failed send closes the connection, so the limit is the longest stall the
  // link survives; 2 s lost recordings to ordinary Wi-Fi hiccups.
  CHECK(hg::ws::kSendTimeoutMs >= 8000);
  CHECK(hg::ws::kNetworkTimeoutMs >= hg::ws::kSendTimeoutMs);
}

TEST("ws: a dead link is still found well before the send task counts as stuck") {
  // A stalled send gives up and the client reports the loss long before the
  // watchdog would restart the device, and within the shortest heartbeat window
  // the app accepts (3 x 5 s).
  CHECK(hg::ws::kSendTimeoutMs * 4 <= hg::ws::kStuckMs);
  CHECK(hg::ws::kNetworkTimeoutMs + hg::ws::kCloseTimeoutMs < hg::ws::kStuckMs);
  CHECK(hg::ws::kSendTimeoutMs <= 3 * 5000);
}

// --- A stuck send task ends in a restart, not a wedge (#136) -----------------

TEST("ws: an idle send task is never stuck") {
  hg::ws::TxWatch w;
  CHECK(!w.stuck(0));
  CHECK(!w.stuck(10 * hg::ws::kStuckMs));
}

TEST("ws: a send task within the limit is not stuck") {
  hg::ws::TxWatch w;
  w.begin(1000);
  CHECK(!w.stuck(1000));
  CHECK(!w.stuck(1000 + hg::ws::kStuckMs));
}

TEST("ws: a send task on one item past the limit is stuck") {
  hg::ws::TxWatch w;
  w.begin(1000);
  CHECK(w.stuck(1001 + hg::ws::kStuckMs));
}

TEST("ws: finishing the item clears it, and each item gets its own limit") {
  hg::ws::TxWatch w;
  w.begin(1000);
  w.end();
  CHECK(!w.stuck(1001 + hg::ws::kStuckMs));
  w.begin(1000 + 2 * hg::ws::kStuckMs);
  CHECK(!w.stuck(1000 + 2 * hg::ws::kStuckMs + 5));
}

TEST("ws: an item that begins just after the caller read the clock is not stuck") {
  // The main loop reads the clock, then asks stuck(); on the other core the send task may
  // begin its next item in between, so since_ is a few milliseconds after now_ms. That must
  // not count as an item busy for ~49 days (it restarted a board two seconds into a recording).
  hg::ws::TxWatch w;
  w.begin(1003);
  CHECK(!w.stuck(1000));
  CHECK(!w.stuck(1002));
  CHECK(w.stuck(1004 + hg::ws::kStuckMs));
}

TEST("ws: the stuck limit holds across the millisecond clock wrapping") {
  hg::ws::TxWatch w;
  w.begin(0xFFFFFF00u);
  CHECK(!w.stuck(0x00000100u));
  CHECK(w.stuck(0xFFFFFF00u + hg::ws::kStuckMs + 1));
}

TEST("ws: no item that ends by itself is taken for stuck") {
  // A send waits for the client's lock, then writes; a close sends its frame the
  // same way, then waits for the client's task, which ends within the network timeout.
  constexpr uint32_t send = 2 * hg::ws::kSendTimeoutMs;
  constexpr uint32_t close = 2 * hg::ws::kCloseTimeoutMs + hg::ws::kNetworkTimeoutMs;
  CHECK(hg::ws::kStuckMs >= 2 * send);
  CHECK(hg::ws::kStuckMs >= 2 * close);
}

// --- The uplink line ----------------------------------------------------------

TEST("ws: the uplink meter's first window starts with the turn, not after the last message") {
  hg::ws::UplinkMeter m;
  hg::ws::UplinkMeter::Report r;
  m.set_on(true, 1000);
  m.item(1, 5, 2000, true, false);
  CHECK(m.take(2000, r));
  CHECK_EQ(r.bytes_per_s, uint32_t(2000));
  m.set_on(false, 2000);  // the turn ends; the send task then waits without a limit
  // A minute later the next turn starts: its first second counts from then.
  m.set_on(true, 62000);
  m.item(3, 40, 48000, true, false);
  CHECK(!m.take(62500, r));  // not a second yet
  CHECK(m.take(63000, r));
  CHECK_EQ(r.bytes_per_s, uint32_t(48000));
  CHECK_EQ(r.msgs, uint32_t(1));
  CHECK_EQ(r.longest_ms, uint32_t(40));
  CHECK_EQ(r.queue_peak, uint32_t(3));
  m.item(1, 10000, 1924, false, true);  // a send that gave up
  CHECK(m.take(64000, r));
  CHECK_EQ(r.failed, uint32_t(1));
  CHECK_EQ(r.bytes_per_s, uint32_t(0));
}

TEST("ws: turning metering on again mid-turn doesn't restart the window") {
  hg::ws::UplinkMeter m;
  hg::ws::UplinkMeter::Report r;
  m.set_on(true, 0);
  m.item(1, 1, 500, true, false);
  m.set_on(true, 600);  // still on: the same window
  m.item(1, 1, 500, true, false);
  CHECK(m.take(1000, r));
  CHECK_EQ(r.msgs, uint32_t(2));
}
