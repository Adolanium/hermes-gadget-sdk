/*
 * C ABI around the device core for the desktop simulator.
 *
 * The simulator runs the production core (protocol, auth, state machine, UI
 * renderer, audio framing) inside a host process. The host supplies the
 * "drivers" through hgsim_host callbacks and feeds events back through the
 * hgsim_* functions. Every function must be called from one thread.
 */
#ifndef HGSIM_H
#define HGSIM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#define HGSIM_API __declspec(dllexport)
#else
#define HGSIM_API __attribute__((visibility("default")))
#endif

#define HGSIM_ABI_VERSION 5

typedef struct hgsim hgsim;

typedef struct hgsim_host {
  void* user;
  /* transport (WebSocket) */
  void (*transport_connect)(void* user, const char* url, const char* subprotocol);
  int (*transport_send_text)(void* user, const char* data, size_t len);
  int (*transport_send_binary)(void* user, const uint8_t* data, size_t len);
  void (*transport_close)(void* user);
  /* display: rows [y0, y1) of the framebuffer changed */
  void (*display_flush)(void* user, int y0, int y1);
  void (*display_backlight)(void* user, int percent);
  /* microphone */
  int (*mic_start)(void* user, uint32_t rate);
  void (*mic_stop)(void* user);
  /* speaker */
  int (*speaker_begin)(void* user, uint32_t rate);
  void (*speaker_write)(void* user, const int16_t* samples, size_t count);
  void (*speaker_end)(void* user);
  void (*speaker_abort)(void* user);
  int (*speaker_busy)(void* user);
  void (*speaker_volume)(void* user, int percent);
  /* storage: get returns the value length, or -1 when missing */
  int (*storage_get)(void* user, const char* key, char* out, size_t cap);
  void (*storage_set)(void* user, const char* key, const char* value);
  void (*storage_erase)(void* user, const char* key);
  /* system */
  uint32_t (*now_ms)(void* user);
  void (*random_bytes)(void* user, uint8_t* out, size_t len);
  void (*log)(void* user, int level, const char* message);
  /* firmware update slot (see hg::Updater); used when config.update_capacity > 0.
   * begin, write and finish return 1 on success. */
  int (*update_begin)(void* user, size_t size);
  int (*update_write)(void* user, const uint8_t* data, size_t len);
  int (*update_finish)(void* user);
  void (*update_abort)(void* user);
  void (*update_restart)(void* user);
  void (*update_confirm)(void* user);
} hgsim_host;

typedef struct hgsim_config {
  // Set both dimensions to zero for a headless device; otherwise 1..2048.
  int width;
  int height;
  int has_mic;
  int has_speaker;
  int has_backlight;
  int has_scroll_buttons;
  uint32_t mic_rate;
  uint32_t speaker_rate;
  const char* board;
  const char* firmware;
  const char* default_name;
  const char* default_server_url;
  const char* default_access_token;
  /* Button names in on-screen hints; NULL keeps the keyboard's SPACE / ESC. */
  const char* talk_label;
  const char* cancel_label;
  int round; /* circular panel (see hg::DisplayInfo::round) */
  int touch; /* the screen is the main input (see hg::TouchGestures) */
  size_t update_capacity; /* bytes the update slot holds; 0 = no over-the-air updates */
  int update_pending;     /* this boot runs an installed update that isn't confirmed yet */
} hgsim_config;

/* Action handler: fill `result_json` (a JSON object) and return 1, or write an
 * error message into it and return 0. */
typedef int (*hgsim_action_fn)(void* user, const char* args_json, char* result_json, size_t cap);

enum { HGSIM_BUTTON_TALK = 0, HGSIM_BUTTON_CANCEL = 1, HGSIM_BUTTON_UP = 2, HGSIM_BUTTON_DOWN = 3 };

HGSIM_API int hgsim_abi_version(void);
HGSIM_API hgsim* hgsim_create(const hgsim_config* config, const hgsim_host* host);
HGSIM_API void hgsim_destroy(hgsim* sim);
HGSIM_API int hgsim_add_action(hgsim* sim, const char* name, const char* description, const char* params_json,
                               hgsim_action_fn fn, void* user);
HGSIM_API void hgsim_begin(hgsim* sim);
HGSIM_API void hgsim_tick(hgsim* sim);

HGSIM_API void hgsim_network(hgsim* sim, int up, const char* detail);
HGSIM_API void hgsim_transport_open(hgsim* sim);
HGSIM_API void hgsim_transport_text(hgsim* sim, const char* data, size_t len);
HGSIM_API void hgsim_transport_binary(hgsim* sim, const uint8_t* data, size_t len);
HGSIM_API void hgsim_transport_closed(hgsim* sim, const char* reason);
HGSIM_API void hgsim_button(hgsim* sim, int button, int pressed);
/* Touchscreen sample in screen pixels; touching = 0 when the finger lifts. */
HGSIM_API void hgsim_touch(hgsim* sim, int touching, int x, int y);
HGSIM_API void hgsim_mic_samples(hgsim* sim, const int16_t* samples, size_t count);
HGSIM_API void hgsim_submit_text(hgsim* sim, const char* text);
HGSIM_API void hgsim_set_sensor(hgsim* sim, const char* name, double value);
HGSIM_API void hgsim_emit_event(hgsim* sim, const char* name, const char* data_json, int notify_agent);

/* Serial-console command; returns the response length (written into out). */
HGSIM_API int hgsim_console(hgsim* sim, const char* line, char* out, size_t cap);
/* Current status as JSON (same as the console "status" command). */
HGSIM_API int hgsim_status(hgsim* sim, char* out, size_t cap);
HGSIM_API const char* hgsim_screen(hgsim* sim);
/* RGB565, native byte order, width*height pixels. */
HGSIM_API const uint16_t* hgsim_framebuffer(hgsim* sim, int* width, int* height);

#ifdef __cplusplus
}
#endif

#endif /* HGSIM_H */
