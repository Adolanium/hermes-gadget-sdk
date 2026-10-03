#include "port.hpp"  // first: pulls in FreeRTOS.h ahead of task.h/queue.h

#include <cstdlib>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/queue.h"

namespace hgp::events {
namespace {

const char* TAG = "hg.events";
QueueHandle_t queue = nullptr;

}  // namespace

void init() { queue = xQueueCreate(48, sizeof(Event)); }

bool post(EventType type, const void* data, size_t len, uint32_t generation, ConsoleRequest* console) {
  Event ev{type, generation, nullptr, len, console};
  if (data && len) {
    // Payloads may be large (WebSocket frames): prefer PSRAM when present.
    ev.data = static_cast<uint8_t*>(heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!ev.data) ev.data = static_cast<uint8_t*>(malloc(len + 1));
    if (!ev.data) {
      ESP_LOGW(TAG, "out of memory for a %u byte event", static_cast<unsigned>(len));
      return false;
    }
    std::memcpy(ev.data, data, len);
    ev.data[len] = 0;
  }
  if (xQueueSend(queue, &ev, pdMS_TO_TICKS(20)) != pdTRUE) {
    ESP_LOGW(TAG, "event queue full; dropping event %d", static_cast<int>(type));
    free(ev.data);
    return false;
  }
  return true;
}

bool receive(Event& out, TickType_t wait) { return xQueueReceive(queue, &out, wait) == pdTRUE; }

void release(Event& ev) {
  free(ev.data);
  ev.data = nullptr;
}

}  // namespace hgp::events
