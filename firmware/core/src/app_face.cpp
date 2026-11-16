// The face's expression, set by Hermes with the face.set action ("look happy") or from the
// console (`face happy 10`). The renderer draws it on the idle, thinking and speaking screens
// (ui.cpp, draw_mark). It lasts for the seconds asked for, or until the user's next turn
// (end_expression). RAM only: a restart forgets it.
#include <cmath>
#include <cstdlib>

#include "hg/app.hpp"

namespace hg {
namespace {

constexpr int64_t kMaxExpressionS = 3600;
// The mark moves for this long after face.set, then stands still: an expression with no
// seconds can stay for hours, and a lit screen should not be redrawn twice a second for it.
constexpr uint32_t kMarkMovesMs = 60000;

std::string expression_names() {
  std::string names;
  for (mascot::Expression e : mascot::kExpressions) names += std::string(names.empty() ? "" : ", ") + mascot::expression_name(e);
  return names;
}

}  // namespace

void App::add_face_actions() {
  if (!hal_.display) return;

  Action set;
  set.name = "face.set";
  set.description =
      "Put an expression on the face on the device's screen: happy, confused, alert or sleepy; neutral clears it. "
      "It shows on the idle, thinking and speaking screens until the user's next turn, or for `seconds`.";
  json::Value props = json::Value::object();
  json::Value expression = json::Value::object();
  json::Value names = json::Value::array();
  for (mascot::Expression e : mascot::kExpressions) names.push(mascot::expression_name(e));
  expression.set("type", "string").set("enum", names).set("description", "The expression to show");
  props.set("expression", expression);
  json::Value seconds = json::Value::object();
  seconds.set("type", "integer").set("minimum", int64_t(1)).set("maximum", kMaxExpressionS).set(
      "description", "Optional: go back to neutral after this many seconds");
  props.set("seconds", seconds);
  json::Value req = json::Value::array();
  req.push("expression");
  set.params.set("type", "object").set("properties", props).set("required", req);
  set.handler = [this](const json::Value& args, json::Value& result, std::string& error) {
    const bool timed = !args["seconds"].is_null();
    // A whole number in range, checked as the number that arrived: 2.9 is refused, not cut to 2,
    // and 1e300 never reaches a conversion to an integer it does not fit.
    const double secs = timed && args["seconds"].is_number() ? args["seconds"].as_number() : 0;
    if (timed && !(secs >= 1 && secs <= static_cast<double>(kMaxExpressionS) && secs == std::floor(secs))) {
      error = "seconds must be between 1 and 3600";
      return false;
    }
    if (!set_expression(args["expression"].str_or(""), timed ? args["seconds"].as_int() : 0, timed, error)) return false;
    result.set("expression", mascot::expression_name(expression_));
    if (expression_timed_) result.set("seconds", args["seconds"].as_int());
    return true;
  };
  add_action(std::move(set));
}

bool App::set_expression(std::string_view name, int64_t seconds, bool timed, std::string& error) {
  mascot::Expression e;
  if (!mascot::parse_expression(name, e)) {
    error = "expression must be one of: " + expression_names();
    return false;
  }
  if (timed && (seconds < 1 || seconds > kMaxExpressionS)) {
    error = "seconds must be between 1 and 3600";
    return false;
  }
  expression_ = e;
  expression_timed_ = timed && e != mascot::Expression::Neutral;
  expression_until_ = now() + static_cast<uint32_t>(seconds) * 1000u;
  expression_since_ = now();
  update_model();
  return true;
}

void App::end_expression() {
  expression_ = mascot::Expression::Neutral;
  expression_timed_ = false;
}

mascot::Expression App::expression_now() {
  // Signed age, like the timer's: the clock wraps, and a late read must not look like it.
  if (expression_timed_ && static_cast<int32_t>(now() - expression_until_) >= 0) end_expression();
  return expression_;
}

bool App::expression_still() const {
  return expression_ != mascot::Expression::Neutral && now() - expression_since_ >= kMarkMovesMs;
}

std::string App::face_command(std::string_view rest) {
  if (!hal_.display) return "@error this device has no screen";
  if (rest.empty()) {
    json::Value v = json::Value::object();
    v.set("expression", mascot::expression_name(expression_now()));
    return "@face " + v.dump();
  }
  const size_t sp = rest.find(' ');
  const std::string secs(sp == std::string_view::npos ? std::string_view() : rest.substr(sp + 1));
  char* end = nullptr;
  const long long seconds = secs.empty() ? 0 : std::strtoll(secs.c_str(), &end, 10);
  if (!secs.empty() && (end == secs.c_str() || *end)) return "@error usage: face [<expression> [seconds]]";
  std::string error;
  if (!set_expression(rest.substr(0, sp), seconds, !secs.empty(), error)) return "@error " + error;
  return std::string("@ok face ") + mascot::expression_name(expression_);
}

}  // namespace hg
