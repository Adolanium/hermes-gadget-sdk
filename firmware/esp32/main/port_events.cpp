#include "port.hpp"  // first: pulls in FreeRTOS.h ahead of task.h/queue.h

#include <cstdlib>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/queue.h"
#if CONFIG_HG_BOARD_CARDPUTER_ADV
#include "hg/protocol.hpp"
#endif

namespace hgp::events {
namespace {

const char* TAG = "hg.events";
QueueHandle_t queue = nullptr;
#if CONFIG_HG_BOARD_CARDPUTER_ADV
SemaphoreHandle_t firmware_slots = nullptr;
bool firmware_frame(EventType type, const void* data, size_t len) {
  return type == EventType::WsBinary && data && len >= hg::proto::kBinaryHeader &&
         *static_cast<const uint8_t*>(data) == static_cast<uint8_t>(hg::proto::Channel::Firmware);
}
#endif

}  // namespace

// 96 deep: TTS bursts land ~13 ms apart at 24 kHz; 48 overflowed while the
// main task was busy draining into the speaker ring, dropping WsBinary frames
// (audible gaps). Still cheap: an Event is a few words, and payloads live on the heap.
void init() {
  queue = xQueueCreate(96, sizeof(Event));
#if CONFIG_HG_BOARD_CARDPUTER_ADV
  // No PSRAM: the sender's 64 KB OTA window can exhaust internal RAM while
  // flash writes stall the app task. Keep at most four 4 KB chunks in flight
  // here; waiting on the receive task lets TCP apply backpressure to Hermes.
  firmware_slots = xSemaphoreCreateCounting(4, 4);
#endif
}

bool post(EventType type, const void* data, size_t len, uint32_t generation, ConsoleRequest* console) {
  Event ev{type, generation, nullptr, len, console};
#if CONFIG_HG_BOARD_CARDPUTER_ADV
  const bool firmware = firmware_frame(type, data, len);
  if (firmware && (!firmware_slots || xSemaphoreTake(firmware_slots, pdMS_TO_TICKS(2000)) != pdTRUE)) {
    ESP_LOGW(TAG, "timed out waiting for an OTA receive slot");
    return false;
  }
#endif
  if (data && len) {
    // Payloads may be large (WebSocket frames): prefer PSRAM when present.
    ev.data = static_cast<uint8_t*>(heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!ev.data) ev.data = static_cast<uint8_t*>(malloc(len + 1));
    if (!ev.data) {
#if CONFIG_HG_BOARD_CARDPUTER_ADV
      if (firmware) xSemaphoreGive(firmware_slots);
#endif
      ESP_LOGW(TAG, "out of memory for a %u byte event", static_cast<unsigned>(len));
      return false;
    }
    std::memcpy(ev.data, data, len);
    ev.data[len] = 0;
  }
  TickType_t wait = pdMS_TO_TICKS(20);
#if CONFIG_HG_BOARD_CARDPUTER_ADV
  if (firmware) wait = pdMS_TO_TICKS(2000);
#endif
  if (xQueueSend(queue, &ev, wait) != pdTRUE) {
#if CONFIG_HG_BOARD_CARDPUTER_ADV
    if (firmware) xSemaphoreGive(firmware_slots);
#endif
    ESP_LOGW(TAG, "event queue full; dropping event %d", static_cast<int>(type));
    free(ev.data);
    return false;
  }
  return true;
}

bool receive(Event& out, TickType_t wait) { return xQueueReceive(queue, &out, wait) == pdTRUE; }

void release(Event& ev) {
#if CONFIG_HG_BOARD_CARDPUTER_ADV
  const bool firmware = firmware_frame(ev.type, ev.data, ev.len);
#endif
  free(ev.data);
  ev.data = nullptr;
#if CONFIG_HG_BOARD_CARDPUTER_ADV
  if (firmware) xSemaphoreGive(firmware_slots);
#endif
}

}  // namespace hgp::events
