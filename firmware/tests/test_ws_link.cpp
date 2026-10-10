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
  CHECK(hg::ws::may_queue_message(hg::ws::kTxReserved + 1));
  CHECK(!hg::ws::may_queue_message(hg::ws::kTxReserved));
  CHECK(!hg::ws::may_queue_message(0));
}

TEST("ws: a full queue of messages still has room to close") {
  size_t free_slots = hg::ws::kTxMessages + hg::ws::kTxReserved, queued = 0;
  while (hg::ws::may_queue_message(free_slots)) {
    --free_slots;
    ++queued;
  }
  CHECK_EQ(queued, hg::ws::kTxMessages);
  CHECK(free_slots >= 1u);  // close() waits for a slot; this one is always there
}

TEST("ws: a message for the current connection is sent with a time limit") {
  FakeOps ops;
  ops.connected = {&client_a};
  CHECK(hg::ws::process(message(&client_a, 1), 1, ops) == Outcome::Sent);
  CHECK(hg::ws::process(message(&client_a, 1, hg::ws::kBinary), 1, ops) == Outcome::Sent);
  CHECK_EQ(ops.calls.size(), 2u);
  CHECK_EQ(ops.calls[0], std::string("text A 5 2000ms"));
  CHECK_EQ(ops.calls[1], std::string("binary A 5 2000ms"));
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
  CHECK_EQ(ops.calls[2], std::string("text B 5 2000ms"));
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
  CHECK(hg::ws::kSendTimeoutMs > 0 && hg::ws::kSendTimeoutMs <= 5000);
  CHECK(hg::ws::kCloseTimeoutMs > 0 && hg::ws::kCloseTimeoutMs <= 5000);
}
