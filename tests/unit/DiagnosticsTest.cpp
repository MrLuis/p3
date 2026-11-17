#include "UnitTest.h"

#include "p3/Basic/Diagnostics.h"
#include "p3/Basic/SourceManager.h"

using namespace p3;

namespace {
const char *Src = "program hello;\n"
                  "begin\n"
                  "  writeln(x)\n"
                  "end.";

SourceLocation locOf(SourceManager &SM, FileID FID, llvm::StringRef Needle) {
  llvm::StringRef Buf = SM.getBufferData(FID);
  return SourceLocation::fromPointer(Buf.data() + Buf.find(Needle));
}
} // namespace

TEST(SourceManager, LineAndColumn) {
  SourceManager SM;
  FileID FID = SM.addBuffer(Src, "hello.pas");
  EXPECT_EQ(FID, 1u);

  PresumedLoc P = SM.getPresumedLoc(locOf(SM, FID, "x)"));
  EXPECT_EQ(P.Filename, llvm::StringRef("hello.pas"));
  EXPECT_EQ(P.Line, 3u);
  EXPECT_EQ(P.Column, 11u);

  P = SM.getPresumedLoc(SM.getStartOfFile(FID));
  EXPECT_EQ(P.Line, 1u);
  EXPECT_EQ(P.Column, 1u);

  // Last line has no trailing newline.
  P = SM.getPresumedLoc(locOf(SM, FID, "end."));
  EXPECT_EQ(P.Line, 4u);
}

TEST(SourceManager, LineText) {
  SourceManager SM;
  FileID FID = SM.addBuffer(Src, "hello.pas");
  EXPECT_EQ(SM.getLineText(locOf(SM, FID, "x)")), llvm::StringRef("  writeln(x)"));
  EXPECT_EQ(SM.getLineText(SM.getStartOfFile(FID)),
            llvm::StringRef("program hello;"));
  EXPECT_EQ(SM.getLineText(locOf(SM, FID, "end.")), llvm::StringRef("end."));
  // A location at a line terminator belongs to the line it ends.
  EXPECT_EQ(SM.getLineText(locOf(SM, FID, "\nbegin")),
            llvm::StringRef("program hello;"));
}

TEST(SourceManager, InvalidLocation) {
  SourceManager SM;
  SM.addBuffer(Src, "hello.pas");
  EXPECT_FALSE(SM.getPresumedLoc(SourceLocation()).isValid());
  EXPECT_EQ(SM.getFileID(SourceLocation()), 0u);
}

TEST(Diagnostics, ErrorWithCaret) {
  SourceManager SM;
  FileID FID = SM.addBuffer(Src, "hello.pas");
  std::string Out;
  llvm::raw_string_ostream OS(Out);
  DiagnosticsEngine D(SM, OS);

  D.error(locOf(SM, FID, "x)"), "undeclared identifier 'x'");
  EXPECT_EQ(Out, std::string("hello.pas:3:11: error: undeclared identifier 'x'\n"
                             "  writeln(x)\n"
                             "          ^\n"));
  EXPECT_EQ(D.getNumErrors(), 1u);
  EXPECT_TRUE(D.hasErrors());
}

TEST(Diagnostics, RangeIsUnderlined) {
  SourceManager SM;
  FileID FID = SM.addBuffer(Src, "hello.pas");
  std::string Out;
  llvm::raw_string_ostream OS(Out);
  DiagnosticsEngine D(SM, OS);

  SourceLocation B = locOf(SM, FID, "writeln");
  D.warning(B, "something", {SourceRange(B, B.getLocWithOffset(7))});
  EXPECT_EQ(Out, std::string("hello.pas:3:3: warning: something\n"
                             "  writeln(x)\n"
                             "  ^~~~~~~\n"));
  EXPECT_EQ(D.getNumWarnings(), 1u);
  EXPECT_FALSE(D.hasErrors());
}

TEST(Diagnostics, NoLocation) {
  SourceManager SM;
  std::string Out;
  llvm::raw_string_ostream OS(Out);
  DiagnosticsEngine D(SM, OS);

  D.error("no input file");
  D.fatal("giving up");
  EXPECT_EQ(Out, std::string("p3: error: no input file\n"
                             "p3: fatal error: giving up\n"));
  EXPECT_TRUE(D.shouldStop());
}

TEST(Diagnostics, WarningsAsErrors) {
  SourceManager SM;
  std::string Out;
  llvm::raw_string_ostream OS(Out);
  DiagnosticsEngine D(SM, OS);
  D.setWarningsAsErrors(true);

  D.warning("unused variable");
  EXPECT_EQ(Out, std::string("p3: error: unused variable\n"));
  EXPECT_EQ(D.getNumErrors(), 1u);
  EXPECT_EQ(D.getNumWarnings(), 0u);
}

TEST(Diagnostics, ErrorLimitStopsAndSuppressesNotes) {
  SourceManager SM;
  std::string Out;
  llvm::raw_string_ostream OS(Out);
  DiagnosticsEngine D(SM, OS);
  D.setErrorLimit(2);

  D.error("e1");
  D.note("n1");
  D.error("e2");
  D.error("e3"); // over the limit -> fatal "too many errors"
  D.note("n3");  // attached to a suppressed error -> dropped
  D.error("e4"); // after stop -> dropped
  EXPECT_EQ(Out, std::string(
                     "p3: error: e1\n"
                     "p3: note: n1\n"
                     "p3: error: e2\n"
                     "p3: fatal error: too many errors emitted, stopping "
                     "now [-ferror-limit=]\n"));
  EXPECT_EQ(D.getNumErrors(), 2u);
  EXPECT_TRUE(D.shouldStop());
}
