// WebSocket transport over esp_websocket_client. Runs on the client's own
// task; complete messages are posted to the app task as events.
#include "port.hpp"  // first: pulls in FreeRTOS.h ahead of task.h/queue.h

#include <cstring>

#include "esp_crt_bundle.h"
#include "esp_log.h"

namespace hgp {
namespace {

const char* TAG = "hg.ws";
constexpr int kBufferSize = 4096;
constexpr size_t kMaxMessage = 512 * 1024;  // images arrive in 4 KB chunks; this only bounds JSON

}  // namespace

void WsTransport::connect(const std::string& url, const std::string& subprotocol) {
  close();
  url_ = url;
  subprotocol_ = subprotocol;
  esp_websocket_client_config_t cfg = {};
  cfg.uri = url_.c_str();
  cfg.subprotocol = subprotocol_.c_str();
  cfg.buffer_size = kBufferSize;
  cfg.task_stack = 6144;
  cfg.disable_auto_reconnect = true;  // hg::App owns retry and backoff
  cfg.network_timeout_ms = 10000;
  cfg.ping_interval_sec = 0;          // the protocol has its own heartbeat
  if (url_.rfind("wss://", 0) == 0) cfg.crt_bundle_attach = esp_crt_bundle_attach;
  client_ = esp_websocket_client_init(&cfg);
  if (!client_) {
    events::post(EventType::WsClosed, "client init failed", 18, generation_.load());
    return;
  }
  esp_websocket_register_events(client_, WEBSOCKET_EVENT_ANY, &WsTransport::on_event, this);
  rx_.clear();
  if (esp_websocket_client_start(client_) != ESP_OK) {
    events::post(EventType::WsClosed, "client start failed", 19, generation_.load());
  }
}

void WsTransport::close() {
  ++generation_;  // anything still queued from the old connection is now stale
  if (!client_) return;
  if (esp_websocket_client_is_connected(client_)) esp_websocket_client_close(client_, pdMS_TO_TICKS(1000));
  esp_websocket_client_destroy(client_);
  client_ = nullptr;
}

bool WsTransport::send_text(std::string_view text) {
  if (!client_ || !esp_websocket_client_is_connected(client_)) return false;
  int n = esp_websocket_client_send_text(client_, text.data(), static_cast<int>(text.size()), pdMS_TO_TICKS(2000));
  return n == static_cast<int>(text.size());
}

bool WsTransport::send_binary(const uint8_t* data, size_t len) {
  if (!client_ || !esp_websocket_client_is_connected(client_)) return false;
  int n = esp_websocket_client_send_bin(client_, reinterpret_cast<const char*>(data), static_cast<int>(len),
                                         pdMS_TO_TICKS(2000));
  return n == static_cast<int>(len);
}

void WsTransport::on_event(void* arg, const char*, int32_t id, void* event_data) {
  auto* self = static_cast<WsTransport*>(arg);
  auto* d = static_cast<esp_websocket_event_data_t*>(event_data);
  const uint32_t gen = self->generation_.load();
  switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
      events::post(EventType::WsOpen, nullptr, 0, gen);
      break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
      events::post(EventType::WsClosed, "disconnected", 12, gen);
      break;
    case WEBSOCKET_EVENT_ERROR:
      events::post(EventType::WsClosed, "connection error", 16, gen);
      break;
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
        events::post(t, self->rx_.data(), self->rx_.size(), gen);
        self->rx_.clear();
      }
      break;
    }
    default:
      break;
  }
}

}  // namespace hgp
