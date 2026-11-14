#include "rx8130.hpp"

#include <initializer_list>

namespace hg {
namespace {

constexpr uint8_t kSeconds = 0x10;  // .. kYear (0x16): seconds, minutes, hours,
                                    // weekday (one bit per day), day, month, year (00-99)
constexpr uint8_t kFlags = 0x1D;
constexpr uint8_t kVoltageLow = 0x02;       // VLF: the time is invalid
constexpr uint8_t kClearVoltageLow = 0xBD;  // flags clear on 0 and ignore 1; bit 6 is always 0
constexpr uint8_t kControl0 = 0x1E;
constexpr uint8_t kStopped = 0xC0;          // TEST | STOP: the clock does not run
constexpr uint16_t kYearBase = 2000;        // the year register holds 00-99

uint8_t to_bcd(uint8_t v) { return static_cast<uint8_t>(((v / 10) << 4) | (v % 10)); }
uint8_t from_bcd(uint8_t v) { return static_cast<uint8_t>(((v >> 4) * 10) + (v & 0x0F)); }
// Both digits 0-9: a table that holds no real time can hold 0xA-0xF.
bool bcd_digits(uint8_t v) { return (v & 0x0F) <= 9 && (v >> 4) <= 9; }

uint8_t days_in_month(uint16_t year, uint8_t month) {  // month 1-12
  static constexpr uint8_t kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
  return month == 2 && leap ? 29 : kDays[month - 1];
}

}  // namespace

bool Rx8130::read(WallTime& utc) {
  // Flags and control first: a time read from a stopped or reset chip would seed a
  // wrong clock until network time corrects it, and waiting for the network is better.
  uint8_t state[2];
  if (!read_(kFlags, state, sizeof(state))) return false;
  if ((state[0] & kVoltageLow) || (state[1] & kStopped)) return false;
  uint8_t r[7];
  if (!read_(kSeconds, r, sizeof(r))) return false;
  const uint8_t second = r[0] & 0x7F, minute = r[1] & 0x7F, hour = r[2] & 0x3F, week = r[3] & 0x7F,
                day = r[4] & 0x3F, month = r[5] & 0x1F;
  for (uint8_t v : {second, minute, hour, day, month, r[6]})
    if (!bcd_digits(v)) return false;
  if (week == 0 || (week & (week - 1)) != 0) return false;  // exactly one day's bit
  uint8_t weekday = 0;
  while (!(week & (1u << weekday))) ++weekday;
  utc.second = from_bcd(second);
  utc.minute = from_bcd(minute);
  utc.hour = from_bcd(hour);
  utc.weekday = weekday;  // 0 = Sunday
  utc.day = from_bcd(day);
  utc.month = from_bcd(month);
  utc.year = static_cast<uint16_t>(kYearBase + from_bcd(r[6]));
  if (utc.second > 59 || utc.minute > 59 || utc.hour > 23 || utc.month < 1 || utc.month > 12) return false;
  return utc.day >= 1 && utc.day <= days_in_month(utc.year, utc.month);
}

bool Rx8130::write(const WallTime& utc) {
  if (utc.year < kYearBase || utc.year > kYearBase + 99 || utc.weekday > 6) return false;
  const uint8_t r[7] = {
      to_bcd(utc.second),  to_bcd(utc.minute), to_bcd(utc.hour),
      static_cast<uint8_t>(1u << utc.weekday),
      to_bcd(utc.day),     to_bcd(utc.month),  to_bcd(static_cast<uint8_t>(utc.year - kYearBase)),
  };
  if (!write_(kSeconds, r, sizeof(r))) return false;
  uint8_t control = 0;
  if (!read_(kControl0, &control, 1)) return false;
  if (control & kStopped) {
    control = static_cast<uint8_t>(control & ~kStopped);
    if (!write_(kControl0, &control, 1)) return false;
  }
  return write_(kFlags, &kClearVoltageLow, 1);
}

}  // namespace hg
