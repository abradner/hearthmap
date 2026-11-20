#include "readings_ingest.h"

#include <cmath>
#include <cstdlib>
#include <string>

namespace hearthmap_device {

namespace {

bool is_space(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

std::string_view trim(std::string_view s) {
  while (!s.empty() && is_space(s.front())) s.remove_prefix(1);
  while (!s.empty() && is_space(s.back())) s.remove_suffix(1);
  return s;
}

bool is_digit(char c) {
  return c >= '0' && c <= '9';
}

// The JSON-ish number grammar: [+-] digits [. digits] [(e|E) [+-] digits], with at least one
// digit in the mantissa. Checked by hand so strtod's extras (hex, "inf", "nan", locale) never
// apply.
bool is_decimal(std::string_view s) {
  std::size_t i = 0;
  if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
  std::size_t digits = 0;
  while (i < s.size() && is_digit(s[i])) ++i, ++digits;
  if (i < s.size() && s[i] == '.') {
    ++i;
    while (i < s.size() && is_digit(s[i])) ++i, ++digits;
  }
  if (digits == 0) return false;
  if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
    ++i;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
    std::size_t exp_digits = 0;
    while (i < s.size() && is_digit(s[i])) ++i, ++exp_digits;
    if (exp_digits == 0) return false;
  }
  return i == s.size();
}

}  // namespace

hearthmap::Reading parse_state(std::string_view state, hearthmap::Timestamp now) {
  hearthmap::Reading r;
  r.last_updated = now;
  if (state == "unavailable") {
    r.state = hearthmap::ReadingState::Unavailable;
    return r;
  }
  r.state = hearthmap::ReadingState::Unknown;
  const std::string_view t = trim(state);
  if (!is_decimal(t)) return r;
  const std::string copy(t);  // strtod needs a terminator
  const double v = std::strtod(copy.c_str(), nullptr);
  if (!std::isfinite(v)) return r;  // e.g. "1e999"
  r.state = hearthmap::ReadingState::Value;
  r.value = v;
  return r;
}

void apply_state(hearthmap::Readings& readings, std::string_view entity_id, std::string_view state,
                 hearthmap::Timestamp now) {
  hearthmap::Reading next = parse_state(state, now);
  auto it = readings.by_entity.find(entity_id);
  if (it == readings.by_entity.end()) {
    readings.by_entity.emplace(std::string(entity_id), std::move(next));
    return;
  }
  next.unit = std::move(it->second.unit);
  it->second = std::move(next);
}

void apply_unit(hearthmap::Readings& readings, std::string_view entity_id, std::string_view unit) {
  auto it = readings.by_entity.find(entity_id);
  if (it == readings.by_entity.end()) {
    hearthmap::Reading r;  // Unknown, last_updated 0
    r.unit = std::string(unit);
    readings.by_entity.emplace(std::string(entity_id), std::move(r));
    return;
  }
  it->second.unit.assign(unit.data(), unit.size());
}

std::size_t restamp(hearthmap::Readings& readings, bool api_connected, hearthmap::Timestamp now) {
  if (!api_connected) return 0;
  std::size_t n = 0;
  for (auto& entry : readings.by_entity) {
    if (entry.second.state == hearthmap::ReadingState::Value) {
      entry.second.last_updated = now;
      ++n;
    }
  }
  return n;
}

}  // namespace hearthmap_device
