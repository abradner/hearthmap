#include "clock_text.h"

#include <cstdint>
#include <cstdio>

namespace hearthmap::host {

std::string clock_text(Timestamp now, int utc_offset_minutes) {
  const std::int64_t local = now + static_cast<std::int64_t>(utc_offset_minutes) * 60;
  std::int64_t of_day = local % 86400;
  if (of_day < 0) of_day += 86400;
  char buf[16];
  std::snprintf(buf, sizeof buf, "%02d:%02d", static_cast<int>(of_day / 3600),
                static_cast<int>((of_day % 3600) / 60));
  return buf;
}

}  // namespace hearthmap::host
