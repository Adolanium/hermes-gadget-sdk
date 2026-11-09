#include <array>
#include <cstdint>

#include "hg/camera.hpp"
#include "check.hpp"

static std::string cam_err;

TEST("camera countdown counts down in whole seconds and fires once at three seconds") {
  hg::Countdown countdown;
  CHECK(!countdown.running());
  CHECK(!countdown.fired());

  countdown.start(1000);
  CHECK(countdown.running());
  CHECK_EQ(countdown.seconds_left(1000), 3);
  countdown.tick(1000);
  CHECK(!countdown.fired());

  countdown.start(1000);
  CHECK_EQ(countdown.seconds_left(1500), 3);
  CHECK_EQ(countdown.seconds_left(1800), 3);  // 1200 ms left rounds up to 2 -> wait
  CHECK_EQ(countdown.seconds_left(2100), 2);
  CHECK_EQ(countdown.seconds_left(3200), 1);
  countdown.tick(3200);
  CHECK(!countdown.fired());

  countdown.tick(4500);  // 3.5 s elapsed: fired
  CHECK(countdown.fired());
  CHECK_EQ(countdown.seconds_left(4500), 0);

  countdown.reset();
  CHECK(!countdown.running());
  CHECK(!countdown.fired());
}

TEST("fake camera serves a capture and a preview of the configured size") {
  struct FakeCamera : hg::Camera {
    bool available() const override { return true; }
    bool draw_preview(uint16_t x, uint16_t y, uint16_t w, uint16_t h) override {
      last_x = x;
      last_y = y;
      last_w = w;
      last_h = h;
      ++preview_draws;
      return !draw_fails;
    }
    bool capture(Still& out, std::string& error) override {
      if (capture_fails) {
        error = "no sensor";
        return false;
      }
      out.width = 640;
      out.height = 480;
      out.pixels.assign(640u * 480u * 2u, 0xAB);
      ++captures;
      return true;
    }

    uint16_t last_x = 0, last_y = 0, last_w = 0, last_h = 0;
    unsigned preview_draws = 0;
    unsigned captures = 0;
    bool draw_fails = false;
    bool capture_fails = false;
  };

  FakeCamera cam;
  CHECK_EQ(cam.preview_draws, 0u);

  CHECK(cam.draw_preview(10, 20, 320, 240));
  CHECK_EQ(cam.preview_draws, 1u);
  CHECK_EQ(cam.last_x, uint16_t(10));
  CHECK_EQ(cam.last_y, uint16_t(20));
  CHECK_EQ(cam.last_w, uint16_t(320));
  CHECK_EQ(cam.last_h, uint16_t(240));

  hg::Camera::Still still;
  CHECK(cam.capture(still, cam_err));
  CHECK_EQ(cam.captures, 1u);
  CHECK_EQ(still.width, uint16_t(640));
  CHECK_EQ(still.height, uint16_t(480));
  CHECK_EQ(still.pixels.size(), size_t(640u * 480u * 2u));

  cam.draw_fails = true;
  cam.preview_draws = 0;
  CHECK(!cam.draw_preview(0, 0, 320, 240));
  CHECK_EQ(cam.preview_draws, 1u);
  cam.capture_fails = true;
  CHECK_EQ(cam.captures, 1u);
  CHECK(!cam.capture(still, cam_err));
  CHECK(!cam_err.empty());
}
