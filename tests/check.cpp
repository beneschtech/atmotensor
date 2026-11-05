#include "check.hpp"

#include <cstdio>
#include <exception>

namespace tst
{

std::vector<std::pair<std::string, TestFn>>& registry()
{
  static std::vector<std::pair<std::string, TestFn>> r;
  return r;
}

int& failures()
{
  static int f = 0;
  return f;
}

void fail(const char* file, int line, const std::string& what)
{
  if (++failures() <= 40)
  {
    std::fprintf(stderr, "  FAIL %s:%d: %s\n", file, line, what.c_str());
  }
}

Reg::Reg(const char* name, TestFn fn)
{
  registry().emplace_back(name, fn);
}

int run_all()
{
  int bad_tests = 0;
  for (auto& t : registry())
  {
    const int before = failures();
    std::printf("[ RUN  ] %s\n", t.first.c_str());
    std::fflush(stdout);
    try
    {
      t.second();
    }
    catch (const std::exception& e)
    {
      fail(t.first.c_str(), 0, std::string("uncaught exception: ") + e.what());
    }
    catch (...)
    {
      fail(t.first.c_str(), 0, "uncaught non-standard exception");
    }
    if (failures() == before)
    {
      std::printf("[  OK  ] %s\n", t.first.c_str());
    }
    else
    {
      std::printf(
        "[ FAIL ] %s (%d failed checks)\n", t.first.c_str(),
        failures() - before);
      ++bad_tests;
    }
  }
  std::printf("\n%zu tests, %d failed\n", registry().size(), bad_tests);
  return bad_tests == 0 ? 0 : 1;
}

}  // namespace tst
