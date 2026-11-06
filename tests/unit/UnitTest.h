//===- UnitTest.h - Minimal self-registering test framework -------*- C++ -*-===//
//
// Deliberately tiny so the project has no third-party test dependency.
//
//   TEST(Suite, Name) { EXPECT_EQ(a, b); EXPECT_TRUE(x); }
//
//===----------------------------------------------------------------------===//
#ifndef PCC_UNITTEST_H
#define PCC_UNITTEST_H

#include "llvm/Support/raw_ostream.h"

#include <functional>
#include <string>
#include <vector>

namespace pcc::unittest {

struct TestCase {
  const char *Suite;
  const char *Name;
  std::function<void()> Fn;
};

std::vector<TestCase> &registry();
void reportFailure(const char *File, int Line, const std::string &Msg);

struct Registrar {
  Registrar(const char *S, const char *N, std::function<void()> F) {
    registry().push_back({S, N, std::move(F)});
  }
};

} // namespace pcc::unittest

#define TEST(SUITE, NAME)                                                      \
  static void SUITE##_##NAME##_impl();                                         \
  static ::pcc::unittest::Registrar SUITE##_##NAME##_reg(#SUITE, #NAME,        \
                                                         SUITE##_##NAME##_impl);\
  static void SUITE##_##NAME##_impl()

#define EXPECT_TRUE(C)                                                         \
  do {                                                                         \
    if (!(C))                                                                  \
      ::pcc::unittest::reportFailure(__FILE__, __LINE__,                       \
                                     "expected true: " #C);                    \
  } while (0)

#define EXPECT_FALSE(C) EXPECT_TRUE(!(C))

#define EXPECT_EQ(A, B)                                                        \
  do {                                                                         \
    auto &&_a = (A);                                                           \
    auto &&_b = (B);                                                           \
    if (!(_a == _b)) {                                                         \
      std::string _m;                                                          \
      llvm::raw_string_ostream _os(_m);                                        \
      _os << "expected " #A " == " #B "\n  lhs: " << _a << "\n  rhs: " << _b;  \
      ::pcc::unittest::reportFailure(__FILE__, __LINE__, _m);                  \
    }                                                                          \
  } while (0)

#endif // PCC_UNITTEST_H
