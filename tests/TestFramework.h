// Minimal test harness. No external dependency, so CI needs nothing fetched to
// run the DSP tests.
#pragma once

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace test
{
inline int gFailures = 0;
inline int gChecks = 0;
inline std::string gCurrentTest;

inline void beginTest (const char* name)
{
    gCurrentTest = name;
    std::printf ("\n=== %s ===\n", name);
}

inline void check (bool ok, const char* expr, const char* file, int line)
{
    ++gChecks;
    if (! ok)
    {
        ++gFailures;
        std::printf ("  FAIL  %s\n        at %s:%d\n", expr, file, line);
    }
}

inline void checkClose (double a, double b, double tol, const char* expr, const char* file, int line)
{
    ++gChecks;
    if (! (std::abs (a - b) <= tol))
    {
        ++gFailures;
        std::printf ("  FAIL  %s\n        %.6g vs %.6g (tol %.3g) at %s:%d\n",
                     expr, a, b, tol, file, line);
    }
}

inline void info (const char* fmt, double v)
{
    std::printf ("  info  ");
    std::printf (fmt, v);
    std::printf ("\n");
}

inline void info2 (const char* fmt, double a, double b)
{
    std::printf ("  info  ");
    std::printf (fmt, a, b);
    std::printf ("\n");
}

inline int summary()
{
    std::printf ("\n----------------------------------------\n");
    std::printf ("%d checks, %d failures\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
} // namespace test

#define CHECK(x)              test::check ((x), #x, __FILE__, __LINE__)
#define CHECK_CLOSE(a, b, t)  test::checkClose ((a), (b), (t), #a " ~= " #b, __FILE__, __LINE__)
#define CHECK_GT(a, b)        test::check ((a) > (b), #a " > " #b, __FILE__, __LINE__)
#define CHECK_LT(a, b)        test::check ((a) < (b), #a " < " #b, __FILE__, __LINE__)
