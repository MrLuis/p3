#include "UnitTest.h"

using namespace p3::unittest;

static unsigned Failures = 0;

std::vector<TestCase> &p3::unittest::registry() {
  static std::vector<TestCase> R;
  return R;
}

void p3::unittest::reportFailure(const char *File, int Line,
                                  const std::string &Msg) {
  ++Failures;
  llvm::errs() << File << ':' << Line << ": FAILURE: " << Msg << '\n';
}

int main() {
  unsigned FailedTests = 0;
  for (const TestCase &T : registry()) {
    unsigned Before = Failures;
    T.Fn();
    bool OK = Failures == Before;
    if (!OK)
      ++FailedTests;
    llvm::outs() << (OK ? "[  OK  ] " : "[ FAIL ] ") << T.Suite << '.'
                 << T.Name << '\n';
  }
  llvm::outs() << registry().size() << " tests, " << FailedTests
               << " failed\n";
  return FailedTests ? 1 : 0;
}
