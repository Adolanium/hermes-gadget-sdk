// The camera mode (the settings menu's Camera item): the core owns the mode, the
// port draws the frames (the #294 plan). A tap starts a three-second countdown,
// the capture holds its photo on screen, and leaving frees the photo's PSRAM.
// The draw step rides the app's render tick: draw_preview of the HAL at the frame
// rate, the countdown number over it, the photo once — all into the panel rows
// between the bars, which the Ui leaves to the camera screen (see ui.cpp).
#include "hg/app.hpp"

#include <algorithm>
#include <utility>

namespace hg {

namespace {

// With nothing pressed, the camera mode leaves itself after this long, so a
// forgotten gadget does not hold the panel and the camera forever.
constexpr uint32_t kCameraIdleMs = 60000;
// The live view refreshes at most once an app frame: about 10 fps on a QVGA
// window, the rate the #294 plan caps the preview at.
constexpr uint32_t kCameraFrameMs = 100;
// The countdown number and its outline, drawn through a Canvas over the live
// view the port just painted.
constexpr uint16_t kInk = 0xFFFF, kOutline = 0x0000;

}  // namespace

// Called from the settings menu's Camera item, or the `camera` console command.
bool App::open_camera() {
  if (camera_open_) return true;
  if (!hal_.camera || !hal_.camera->available() || !hal_.display || !ui_ || hal_.display->framebuffer_rows() > 0)
    return false;
  if (face_down_ || alarm_ || prompt_showing() || ota_busy() || ota_ == Ota::Restarting || !wifi_setup_text_.empty() ||
      phone_pairing_)
    return false;
  if (settings_open()) close_settings();
  if (mode_ == Mode::Listening) cancel_listening("camera");
  else if (mode_ != Mode::Idle) cancel_turn();
  stop_playback();
  dismiss_overlay();
  wake_display();
  talk_held_ = false;
  cancel_held_ = false;
  settings_chord_fired_ = false;
  wake_buttons_ = 0;
  camera_open_ = true;
  camera_state_ = CameraState::Preview;
  camera_countdown_.reset();
  camera_shot_ = {};
  camera_shot_drawn_ = false;
  camera_touch_at_ = now();
  camera_frame_due_ = true;  // the first frame goes out with this very pass
  ui_->invalidate();
  update_model();
  return true;
}

void App::close_camera() {
  if (!camera_open_) return;
  camera_open_ = false;
  camera_state_ = CameraState::Preview;
  camera_countdown_.reset();
  camera_shot_ = {};  // the photo's PSRAM goes back
  camera_shot_drawn_ = false;
  if (ui_) ui_->invalidate();  // the camera owned the panel rows between the bars
  update_model();
}

// A tap anywhere: start the countdown, cancel one under way, or look again.
void App::camera_tap() {
  if (!camera_open_) return;
  camera_touch_at_ = now();
  switch (camera_state_) {
    case CameraState::Preview:
      camera_state_ = CameraState::Countdown;
      camera_countdown_.start(now());
      break;
    case CameraState::Countdown:
      // A second tap takes the frame back: the countdown waits for no one.
      camera_countdown_.reset();
      camera_state_ = CameraState::Preview;
      break;
    case CameraState::Shot:
      // Retake: the live view comes back first; the next tap counts down.
      camera_state_ = CameraState::Preview;
      camera_shot_ = {};
      camera_shot_drawn_ = false;
      camera_frame_due_ = true;
      break;
  }
  update_model();
}

// The countdown's fire, and the 60-second idle leave. Runs from App::tick().
void App::camera_tick() {
  if (!camera_open_) return;
  if (camera_state_ == CameraState::Countdown) {
    camera_countdown_.tick(now());
    if (camera_countdown_.fired()) {
      camera_capture();
      return;
    }
    update_model();  // the number and the hint follow the seconds
    return;
  }
  if (static_cast<int32_t>(now() - camera_touch_at_) >= static_cast<int32_t>(kCameraIdleMs)) {
    log(LogLevel::Info, "camera mode left after 60 s with no touch");
    close_camera();
  }
}

// The countdown fired: one still from the same camera instance, its photo then
// held on screen. A failed capture goes back to the live view with a hint.
void App::camera_capture() {
  camera_countdown_.reset();
  Camera::Still still;
  std::string error;
  if (hal_.camera->capture(still, error)) {
    camera_shot_ = std::move(still);
    camera_state_ = CameraState::Shot;
    camera_shot_drawn_ = false;
  } else {
    log(LogLevel::Warn, "camera capture failed: " + error);
    camera_state_ = CameraState::Preview;
    camera_frame_due_ = true;
    set_hint_flash(tr(i18n::Msg::CameraFailed));
  }
  update_model();
}

// What the camera own of the panel this frame: the live view through the HAL at
// the frame rate, the countdown number over it, the photo once, none after.
void App::camera_draw() {
  if (!ui_ || !hal_.camera || !hal_.display || display_sleeping_) return;
  const UiLayout& l = ui_->layout();
  const int w = ui_->area().width;
  if (camera_state_ == CameraState::Shot) {
    if (camera_shot_drawn_ || camera_shot_.pixels.empty()) return;
    // A still whose size doesn't match its pixels (a port bug) is not drawn: the
    // scaling below divides by the size and reads rows by it.
    if (camera_shot_.width == 0 || camera_shot_.height == 0 ||
        camera_shot_.pixels.size() < static_cast<size_t>(camera_shot_.width) * camera_shot_.height * 2u)
      return;
    camera_shot_drawn_ = true;
    Canvas c = ui_->canvas();
    c.set_clip_rows(l.main_y, l.main_y + l.main_h);
    c.fill_rect(0, l.main_y, w, l.main_h, kOutline);
    // The photo, fitted to the window with its shape kept, centred on black.
    const int sw = camera_shot_.width, sh = camera_shot_.height;
    const int dw = std::max<int>(1, std::min<int>(w, static_cast<int64_t>(sw) * l.main_h / sh));
    const int dh = std::max<int>(1, static_cast<int64_t>(dw) * sh / sw);
    const int x0 = (w - dw) / 2, y0 = l.main_y + (l.main_h - dh) / 2;
    const uint16_t* px = reinterpret_cast<const uint16_t*>(camera_shot_.pixels.data());
    for (int dy = 0; dy < dh; ++dy) {
      const uint16_t* row = px + static_cast<size_t>(dy * sh / dh) * sw;
      for (int dx = 0; dx < dw; ++dx) c.pixel(x0 + dx, y0 + dy, row[dx * sw / dw]);
    }
    ui_->flush(l.main_y, l.main_y + l.main_h);
    return;
  }
  // A flag, not camera_frame_at_ = 0: past 2^31 ms of uptime (24.8 days), now() - 0 reads
  // as negative and the live view would never draw again.
  if (!camera_frame_due_ && now() - camera_frame_at_ < kCameraFrameMs) return;
  // No frame this call: leave the panel as it is and try again on the next one.
  // Drawing the countdown digit over a stale frame would ghost it into the next
  // digit's shape, and a failed draw should not spend the frame budget.
  if (!hal_.camera->draw_preview(0, static_cast<uint16_t>(l.main_y), static_cast<uint16_t>(w),
                                 static_cast<uint16_t>(l.main_h)))
    return;
  camera_frame_at_ = now();
  camera_frame_due_ = false;
  if (camera_state_ != CameraState::Countdown) return;
  // The big number stays over every new frame: an outlined digit, centred.
  const int scale = 3;
  const std::string number = std::to_string(camera_countdown_.seconds_left(now()));
  const int num_w = Canvas::text_width(number, scale), num_h = Canvas::line_height(scale);
  const int x = std::max(0, (w - num_w) / 2), y = l.main_y + std::max(0, (l.main_h - num_h) / 2);
  Canvas c = ui_->canvas();
  c.set_clip_rows(l.main_y, l.main_y + l.main_h);
  for (int dy = -scale; dy <= scale; dy += scale)
    for (int dx = -scale; dx <= scale; dx += scale)
      if (dx || dy) c.text(x + dx, y + dy, number, scale, kOutline);
  c.text(x, y, number, scale, kInk);
  ui_->flush(y, y + num_h);
}

void App::camera_model() {
  UiModel& m = model_;
  m.screen = Screen::Camera;
  m.headline = tr(i18n::Msg::Camera);
  m.scroll = 0;
  m.speaking = false;
  switch (camera_state_) {
    case CameraState::Preview: m.hint = tr(i18n::Msg::CameraTake); break;
    case CameraState::Countdown: m.hint = tr(i18n::Msg::CameraHoldStill); break;
    case CameraState::Shot: m.hint = tr(i18n::Msg::CameraRetake); break;
  }
  if (!hint_flash_.empty()) m.hint = hint_flash_;
}

}  // namespace hg
