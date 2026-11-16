// The log copy that outlives a restart (firmware/drivers/log_ring.hpp).
#include <cstring>
#include <string>

#include "check.hpp"
#include "log_ring.hpp"

namespace {

template <size_t N>
void write(hg::LogRing<N>& ring, const std::string& text) {
  for (char c : text) ring.put(c);
}

}  // namespace

TEST("log ring: a restart that kept RAM hands the previous boot's lines to the next") {
  hg::LogRing<64> ring;
  std::memset(&ring, 0xa5, sizeof(ring));  // RAM as a cold boot leaves it
  CHECK_EQ(ring.adopt(true), std::string());  // nothing valid to keep
  write(ring, "I (10) boot\nE (900) task_wdt: stuck\n");
  CHECK_EQ(ring.adopt(true), std::string("I (10) boot\nE (900) task_wdt: stuck\n"));
  CHECK_EQ(ring.contents(), std::string());  // this boot starts empty
}

TEST("log ring: after power loss or a torn header the old text is dropped") {
  hg::LogRing<64> ring;
  ring.adopt(false);
  write(ring, "I (10) boot\n");
  CHECK_EQ(ring.adopt(false), std::string());  // power-on: RAM can't be trusted

  write(ring, "I (10) boot\n");
  ring.head = 5;  // reset mid-update: head moved but the seal didn't
  CHECK_EQ(ring.adopt(true), std::string());
}

TEST("log ring: once full it keeps the newest text and drops the cut-off oldest line") {
  hg::LogRing<16> ring;
  ring.adopt(false);
  write(ring, "aaaa\nbbbb\ncccc\ndddd\n");  // 20 bytes into 16: "aaaa" is overwritten
  CHECK_EQ(ring.contents(), std::string("bbbb\ncccc\ndddd\n"));
  CHECK_EQ(ring.adopt(true), std::string("bbbb\ncccc\ndddd\n"));
}
