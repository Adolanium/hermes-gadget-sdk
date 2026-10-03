// Tiny self-registering test harness (no external dependencies).
#pragma once

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace check {

struct Case {
  const char* name;
  std::function<void()> fn;
};

inline std::vector<Case>& registry() {
  static std::vector<Case> cases;
  return cases;
}

inline int& failures() {
  static int n = 0;
  return n;
}

struct Register {
  Register(const char* name, std::function<void()> fn) { registry().push_back({name, std::move(fn)}); }
};

inline void fail(const char* file, int line, const std::string& what) {
  ++failures();
  std::printf("  FAIL %s:%d: %s\n", file, line, what.c_str());
}

inline int run_all() {
  int failed_cases = 0;
  for (auto& c : registry()) {
    int before = failures();
    c.fn();
    bool ok = failures() == before;
    if (!ok) ++failed_cases;
    std::printf("%s %s\n", ok ? "[ ok ]" : "[FAIL]", c.name);
  }
  std::printf("\n%zu cases, %d failed\n", registry().size(), failed_cases);
  return failed_cases ? 1 : 0;
}

}  // namespace check

#define CHECK_CAT2(a, b) a##b
#define CHECK_CAT(a, b) CHECK_CAT2(a, b)
#define TEST(name)                                                              \
  static void CHECK_CAT(test_fn_, __LINE__)();                                  \
  static check::Register CHECK_CAT(test_reg_, __LINE__)(name, CHECK_CAT(test_fn_, __LINE__)); \
  static void CHECK_CAT(test_fn_, __LINE__)()

#define CHECK(cond)                                    \
  do {                                                 \
    if (!(cond)) check::fail(__FILE__, __LINE__, #cond); \
  } while (0)

#define CHECK_EQ(a, b)                                                                   \
  do {                                                                                   \
    auto _va = (a);                                                                      \
    auto _vb = (b);                                                                      \
    if (!(_va == _vb)) check::fail(__FILE__, __LINE__, std::string(#a " == " #b));      \
  } while (0)
