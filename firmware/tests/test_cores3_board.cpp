#include "board.hpp"
#include "check.hpp"

TEST("CoreS3 uses the BSP's BGR order with its native landscape geometry") {
  const auto& lcd = hgp::board_config().lcd;
  CHECK(lcd.controller == hgp::LcdController::CoreS3);
  CHECK(lcd.bgr);
  CHECK_EQ(lcd.width, 320);
  CHECK_EQ(lcd.height, 240);
  CHECK(!lcd.swap_xy && !lcd.mirror_x && !lcd.mirror_y);
  CHECK(lcd.invert);
}
