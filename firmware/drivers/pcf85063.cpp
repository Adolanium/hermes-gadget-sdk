#include "pcf85063.hpp"

#include <initializer_list>

namespace hg {
namespace {

constexpr uint8_t kSeconds = 0x04;  // .. kYears (0x0A): seconds, minutes, hours,
                                    // days, weekdays, months, years (00-99)
constexpr uint8_t kOscillatorStopped = 0x80;  // seconds bit 7: the time is invalid
constexpr uint16_t kYearBase = 2000;          // the years register holds 00-99

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

bool Pcf85063::read(WallTime& utc) {
  uint8_t r[7];
  if (!read_(kSeconds, r, sizeof(r))) return false;
  if (r[0] & kOscillatorStopped) return false;
  const uint8_t second = r[0] & 0x7F, minute = r[1] & 0x7F, hour = r[2] & 0x3F, day = r[3] & 0x3F,
                month = r[5] & 0x1F;
  // A table that holds no real time would seed a wrong clock until network time
  // corrects it: waiting for the network is better.
  for (uint8_t v : {second, minute, hour, day, month, r[6]})
    if (!bcd_digits(v)) return false;
  utc.second = from_bcd(second);
  utc.minute = from_bcd(minute);
  utc.hour = from_bcd(hour);  // 24 h mode, the reset default (Control_1's 12_24 bit clear)
  utc.day = from_bcd(day);
  utc.weekday = static_cast<uint8_t>(r[4] & 0x07);  // plain 0-6, 0 = Sunday
  utc.month = from_bcd(month);
  utc.year = static_cast<uint16_t>(kYearBase + from_bcd(r[6]));
  if (utc.second > 59 || utc.minute > 59 || utc.hour > 23 || utc.weekday > 6 || utc.month < 1 ||
      utc.month > 12)
    return false;
  return utc.day >= 1 && utc.day <= days_in_month(utc.year, utc.month);
}

bool Pcf85063::write(const WallTime& utc) {
  if (utc.year < kYearBase || utc.year > kYearBase + 99) return false;
  const uint8_t r[7] = {
      to_bcd(utc.second),  // OS = 0: the burst restarts the clock
      to_bcd(utc.minute), to_bcd(utc.hour),          // bit 7 = 0: 24 h mode
      to_bcd(utc.day),     static_cast<uint8_t>(utc.weekday & 0x07),
      to_bcd(utc.month),   to_bcd(static_cast<uint8_t>(utc.year - kYearBase)),
  };
  return write_(kSeconds, r, sizeof(r));
}

int64_t days_from_civil(uint16_t year, uint8_t month, uint8_t day) {
  // Howard Hinnant's algorithm (public domain, credited in NOTICE): era-based, valid for the
  // RTC's 2000-2099 range and far beyond.
  int64_t y = year;
  y -= month <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const int64_t yoe = y - era * 400;                                   // [0, 399]
  const int64_t doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;  // [0, 365]
  const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;           // [0, 146096]
  return era * 146097 + doe - 719468;
}

}  // namespace hg
