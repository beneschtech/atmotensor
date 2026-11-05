// Minimal test harness (no external dependencies).  Checks are non-fatal:
// a failing CHECK is reported and the test keeps going.  An exception that
// escapes a test fails that test and the run continues.
#pragma once

#include <cmath>
#include <string>
#include <utility>
#include <vector>

namespace tst
{

using TestFn = void (*)();

std::vector<std::pair<std::string, TestFn>>& registry();
int& failures();
void fail(const char* file, int line, const std::string& what);
int run_all();

struct Reg
{
  Reg(const char* name, TestFn fn);
};

}  // namespace tst

#define TEST(name)                            \
  static void name();                         \
  static ::tst::Reg reg_##name(#name, &name); \
  static void name()

#define CHECK(cond)                           \
  do                                          \
  {                                           \
    if (!(cond))                              \
    {                                         \
      ::tst::fail(__FILE__, __LINE__, #cond); \
    }                                         \
  } while (0)

#define CHECK_NEAR(a, b, tol)                                           \
  do                                                                    \
  {                                                                     \
    const double va_ = static_cast<double>(a);                          \
    const double vb_ = static_cast<double>(b);                          \
    if (!(std::fabs(va_ - vb_) <= (tol)))                               \
    {                                                                   \
      ::tst::fail(                                                      \
        __FILE__, __LINE__,                                             \
        std::string(#a " ~ " #b "  (") + std::to_string(va_) + " vs " + \
          std::to_string(vb_) + ")");                                   \
    }                                                                   \
  } while (0)
