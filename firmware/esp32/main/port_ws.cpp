// WebSocket transport over esp_websocket_client. Receiving runs on the client's
// own task and sending on hg-ws-tx; complete messages are posted to the app
// task as events.
#include "port.hpp"  // first: pulls in FreeRTOS.h ahead of task.h/queue.h

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_private/esp_clk.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "link_diag.hpp"

namespace hgp {
namespace {

const char* TAG = "hg.ws";
constexpr int kBufferSize = 4096;
constexpr size_t kMaxMessage = 512 * 1024;  // images arrive in 4 KB chunks; this only bounds JSON

using hg::ws::TxItem;

// The client calls hg::ws::process() makes, each with a time limit.
struct EspOps {
  static esp_websocket_client_handle_t h(void* c) { return static_cast<esp_websocket_client_handle_t>(c); }
  bool is_connected(void* c) { return esp_websocket_client_is_connected(h(c)); }
  int send(void* c, uint8_t opcode, const uint8_t* data, size_t len, uint32_t timeout_ms) {
    const auto* p = reinterpret_cast<const char*>(data);
    return opcode == hg::ws::kBinary ? esp_websocket_client_send_bin(h(c), p, static_cast<int>(len), pdMS_TO_TICKS(timeout_ms))
                                     : esp_websocket_client_send_text(h(c), p, static_cast<int>(len), pdMS_TO_TICKS(timeout_ms));
  }
  // Not esp_websocket_client_close(): it ignores its timeout for the close frame.
  void send_close(void* c, uint32_t timeout_ms) {
    esp_websocket_client_send_with_opcode(h(c), WS_TRANSPORT_OPCODES_CLOSE, nullptr, 0, pdMS_TO_TICKS(timeout_ms));
  }
  void destroy(void* c) { esp_websocket_client_destroy(h(c)); }
};

uint32_t now_ms() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

// The access point's signal now, 0 when not joined: lost connections log it.
int rssi_now() {
  wifi_ap_record_t ap = {};
  return esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
}


uint32_t generation_of(const esp_websocket_event_data_t* d) {
  return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(d->user_context));
}

}  // namespace

void WsTransport::connect(const std::string& url, const std::string& subprotocol) {
  close();
  if (!tx_) {
    tx_ = xQueueCreate(hg::ws::kTxMessages + hg::ws::kTxReserved, sizeof(TxItem));
    if (!tx_ || xTaskCreate(&WsTransport::tx_task, "hg-ws-tx", 6144, this, 5, nullptr) != pdPASS) {
      ESP_LOGE(TAG, "can't start the send task");
      if (tx_) vQueueDelete(tx_);
      tx_ = nullptr;
      events::post(EventType::WsClosed, "client init failed", 18, gens_.current());
      return;
    }
  }
  url_ = url;
  subprotocol_ = subprotocol;
  esp_websocket_client_config_t cfg = {};
  cfg.uri = url_.c_str();
  cfg.subprotocol = subprotocol_.c_str();
  cfg.buffer_size = kBufferSize;
  cfg.task_stack = 6144;
  cfg.disable_auto_reconnect = true;  // hg::App owns retry and backoff
  cfg.network_timeout_ms = hg::ws::kNetworkTimeoutMs;
  cfg.ping_interval_sec = 0;          // the protocol has its own heartbeat
  // Every event names the connection it belongs to: a client still being torn
  // down on the send task must not be mistaken for the current one.
  cfg.user_context = reinterpret_cast<void*>(static_cast<uintptr_t>(gens_.current()));
  if (url_.rfind("wss://", 0) == 0) cfg.crt_bundle_attach = esp_crt_bundle_attach;
  client_ = esp_websocket_client_init(&cfg);
  if (!client_) {
    events::post(EventType::WsClosed, "client init failed", 18, gens_.current());
    return;
  }
  esp_websocket_register_events(client_, WEBSOCKET_EVENT_ANY, &WsTransport::on_event, this);
  if (esp_websocket_client_start(client_) != ESP_OK) {
    events::post(EventType::WsClosed, "client start failed", 19, gens_.current());
  }
}

void WsTransport::close() {
  gens_.leave();  // anything still queued from the old connection is now stale
  if (!client_) return;
  // The send task closes and destroys the client after whatever it is sending:
  // on a dead link that can take seconds, and the app task must not wait.
  TxItem item{client_, 0, hg::ws::kClose, nullptr, 0};
  client_ = nullptr;
  // The reserved slots keep room for this. If even they are full, the send task
  // is stuck: the client is left behind and stuck() asks for a restart.
  if (xQueueSend(tx_, &item, pdMS_TO_TICKS(hg::ws::kHandoffTimeoutMs)) != pdTRUE) {
    ESP_LOGE(TAG, "the send task took no close for %u ms", static_cast<unsigned>(hg::ws::kHandoffTimeoutMs));
    handoff_failed_ = true;
  }
}

bool WsTransport::stuck() const { return handoff_failed_ || watch_.stuck(now_ms()); }

bool WsTransport::enqueue(uint8_t opcode, const void* data, size_t len) {
  if (!client_ || !esp_websocket_client_is_connected(client_)) return false;
  if (!hg::ws::may_queue_message(uxQueueSpacesAvailable(tx_), opcode)) {  // the link isn't keeping up
    ++refused_;
    return false;
  }
  auto* copy = static_cast<uint8_t*>(heap_caps_malloc(len ? len : 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!copy) copy = static_cast<uint8_t*>(malloc(len ? len : 1));
  if (!copy) return false;
  if (len) std::memcpy(copy, data, len);
  TxItem item{client_, gens_.current(), opcode, copy, len};
  if (xQueueSend(tx_, &item, 0) != pdTRUE) {
    free(copy);
    return false;
  }
  return true;
}

bool WsTransport::send_text(std::string_view text) { return enqueue(hg::ws::kText, text.data(), text.size()); }

bool WsTransport::send_binary(const uint8_t* data, size_t len) { return enqueue(hg::ws::kBinary, data, len); }

void WsTransport::tx_task(void* arg) {
  auto* self = static_cast<WsTransport*>(arg);
  EspOps ops;
  TxItem item;
  hg::ws::UplinkMeter meter;
  for (;;) {
    // Wakes at least once a second while metering, so a stalled link still gets its line.
    const bool got = xQueueReceive(self->tx_, &item, self->metering_ ? pdMS_TO_TICKS(1000) : portMAX_DELAY) == pdTRUE;
    const uint32_t began = now_ms();
    // A turn starting begins a fresh window, before its first message counts (refusals too).
    const bool on = self->metering_;
    if (on && !meter.on()) self->refused_ = 0;
    meter.set_on(on, began);
    if (got) {
      const uint32_t depth = uxQueueMessagesWaiting(self->tx_) + 1;
      self->watch_.begin(began);
      // A send that fails makes the client close the connection from inside the send, so the
      // close event (on_event) learns of it from here: what is in flight, and since when.
      self->sending_bytes_ = item.len;
      self->sending_since_ = began;
      self->sending_gen_ = item.opcode == hg::ws::kClose ? 0 : item.generation;
      const auto outcome = hg::ws::process(item, self->gens_.current(), ops);
      self->sending_gen_ = 0;
      self->watch_.end();
      const uint32_t took = now_ms() - began;
      if (outcome == hg::ws::Outcome::Failed)
        ESP_LOGW(TAG, "send of %u B gave up after %u ms (rssi %d)", static_cast<unsigned>(item.len),
                 static_cast<unsigned>(took), rssi_now());
      meter.item(depth, took, item.len, outcome == hg::ws::Outcome::Sent, outcome == hg::ws::Outcome::Failed);
      free(const_cast<uint8_t*>(item.data));
    }
    hg::ws::UplinkMeter::Report r;
    if (meter.on() && meter.take(now_ms(), r)) {
      const uint32_t refused = self->refused_.exchange(0);
      if (r.msgs || r.failed || refused)
        ESP_LOGI(TAG, "uplink: %u B/s in %u msgs, longest send %u ms, queue peak %u/%u, refused %u, failed %u, cpu %d MHz, rssi %d",
                 static_cast<unsigned>(r.bytes_per_s), static_cast<unsigned>(r.msgs), static_cast<unsigned>(r.longest_ms),
                 static_cast<unsigned>(r.queue_peak), static_cast<unsigned>(hg::ws::kTxMessages + hg::ws::kTxReserved),
                 static_cast<unsigned>(refused), static_cast<unsigned>(r.failed), esp_clk_cpu_freq() / 1000000, rssi_now());
    }
  }
}

/** Reassemble this connection's frames and hand completed messages to the app.
 * Cardputer delivery detaches payload storage before its bounded OTA wait; old
 * generations are still rejected by dispatch after asynchronous client close.
 */
void WsTransport::on_event(void* arg, const char*, int32_t id, void* event_data) {
  auto* self = static_cast<WsTransport*>(arg);
  auto* d = static_cast<esp_websocket_event_data_t*>(event_data);
  const uint32_t gen = generation_of(d);
  if (!self->gens_.is_current(gen)) return;  // a client being torn down
  switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
      self->connected_gen_ = gen;  // this client's connect attempt is over
      events::post(EventType::WsOpen, nullptr, 0, gen);
      break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
    case WEBSOCKET_EVENT_ERROR: {
      self->gens_.lost(gen);
      // Why, as far as anyone knows: a send that gave up, the client's error, the server's
      // close code, and the signal (hg::link::ws_closed). The app logs it and keeps it for diag.
      // The client fills the socket and TLS fields only for a transport error; the rest may be
      // left over or uninitialised. The upgrade's HTTP status explains only a failed connect
      // attempt (hg::link::upgrade_status): this client never reached CONNECTED.
      hg::link::WsError e;
      e.error_type = d->error_handle.error_type;
      if (e.error_type == WEBSOCKET_ERROR_TYPE_TCP_TRANSPORT) {
        e.sock_errno = d->error_handle.esp_transport_sock_errno;
        e.tls_err = d->error_handle.esp_tls_last_esp_err;
      }
      e.http_status = hg::link::upgrade_status(d->error_handle.esp_ws_handshake_status_code,
                                               self->connected_gen_.load() != gen);
      e.close_code = d->close_status_code;  // esp_websocket_client 1.8.0 and later
      // Lost while a send of this connection was in flight: that send is what gave up.
      hg::link::SendFailure send;
      const bool sent_failed = gen && self->sending_gen_.load() == gen;
      if (sent_failed) {
        send.took_ms = now_ms() - self->sending_since_.load();
        send.bytes = self->sending_bytes_.load();
      }
      const std::string why = hg::link::ws_closed(id == WEBSOCKET_EVENT_ERROR ? "connection error" : "disconnected", e,
                                                  sent_failed ? &send : nullptr, rssi_now());
      ESP_LOGW(TAG, "%s", why.c_str());
      events::post(EventType::WsClosed, why.data(), why.size(), gen);
      break;
    }
    case WEBSOCKET_EVENT_DATA: {
      uint8_t op = d->op_code;
      if (op == 0x8 || op == 0x9 || op == 0xA) break;  // close/ping/pong are handled by the client
      if (op != 0x0) self->rx_opcode_ = op;          // first frame of a message
      if (d->payload_offset == 0 && op != 0x0) self->rx_.clear();
      if (self->rx_.size() + static_cast<size_t>(d->data_len) > kMaxMessage) {
        ESP_LOGW(TAG, "dropping oversized message");
        self->rx_.clear();
        break;
      }
      self->rx_.append(d->data_ptr, static_cast<size_t>(d->data_len));
      bool frame_done = d->payload_offset + d->data_len >= d->payload_len;
      if (frame_done && d->fin) {
        EventType t = self->rx_opcode_ == 0x2 ? EventType::WsBinary : EventType::WsText;
#if CONFIG_HG_BOARD_CARDPUTER_ADV
        // OTA admission can wait for RAM. Do not borrow shared rx_ storage
        // across that wait or clear a replacement client's frame afterward.
        hg::ws::deliver_owned_message(self->rx_, [t, gen](const uint8_t* data, size_t len) {
          return events::post(t, data, len, gen);
        });
#else
        events::post(t, self->rx_.data(), self->rx_.size(), gen);
        self->rx_.clear();
#endif
      }
      break;
    }
    default:
      break;
  }
}

}  // namespace hgp
