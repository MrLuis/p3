// Replays selfhost/tests/t_diag.pas through the C++ DiagnosticsEngine and
// checks that it prints exactly selfhost/tests/t_diag.expected: the
// self-hosted compiler's diagnostics (selfhost/pcdiag.pas) must match the
// C++ compiler's byte for byte. Keep the two sequences in sync.
#include "UnitTest.h"

#include "p3/Basic/Diagnostics.h"
#include "p3/Basic/SourceManager.h"

#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

using namespace p3;

TEST(SelfhostDiag, MatchesPascalPort) {
  std::string Dir = P3_SOURCE_DIR "/selfhost/tests/";
  auto Input = llvm::MemoryBuffer::getFile(Dir + "Inputs/diag.txt");
  auto Expected = llvm::MemoryBuffer::getFile(Dir + "t_diag.expected");
  EXPECT_TRUE(bool(Input));
  EXPECT_TRUE(bool(Expected));
  if (!Input || !Expected)
    return;

  SourceManager SM;
  FileID FID = SM.addBuffer((*Input)->getBuffer(), "diag.txt");
  llvm::StringRef Buf = SM.getBufferData(FID);
  auto Find = [&](llvm::StringRef S) {
    return SourceLocation::fromPointer(Buf.data() + Buf.find(S));
  };
  auto Range = [](SourceLocation B, SourceLocation E) { return SourceRange(B, E); };
  auto Off = [](SourceLocation L, int N) { return L.getLocWithOffset(N); };

  std::string Out;
  llvm::raw_string_ostream OS(Out);
  DiagnosticsEngine D(SM, OS);
  auto Presumed = [&](SourceLocation L) {
    PresumedLoc P = SM.getPresumedLoc(L);
    OS << "presumed " << P.Line << ':' << P.Column << '\n';
  };

  SourceLocation X = Find("x :="), Y = Find("y;"), B = Find("b +"),
                 C = Find("c;"), Z = Find("zzz"), A = Find("a :="),
                 Caf = Find("caf");
  SourceLocation Eof = SourceLocation::fromPointer(Buf.data() + Buf.size());

  Presumed(SM.getStartOfFile(FID));
  Presumed(Y);
  Presumed(C);
  Presumed(Eof);

  D.error(Y, "use of undeclared identifier 'y'",
          {Range(X, Off(X, 1)), Range(Y, Off(Y, 1))});
  D.warning(Caf, "non-ASCII line", {Range(Caf, Find(":= 1"))});
  D.note(B, "range over two lines", {Range(B, Off(C, 1))});
  D.warning("unused-variable", A, "variable 'a' is declared but never used",
            {Range(A, Off(A, 1))});
  D.disableWarning("unused-variable");
  D.warning("unused-variable", C, "disabled: not shown");
  D.error(Eof, "unexpected end of file");
  D.error(Off(Z, 290), "far into a long line", {Range(Z, Off(Z, 300))});
  D.error("cannot open file 'nope.pas'");
  D.note(C, "note after an error");
  D.setWarningsAsErrors(true);
  D.warning(C, "promoted to an error");
  D.setWarningsAsErrors(false);
  D.setErrorLimit(D.getNumErrors() + 1);
  D.error(X, "last error");
  D.error(Y, "over the limit");
  D.note(Y, "suppressed note");
  D.warning(Y, "suppressed warning");
  OS << "errors: " << D.getNumErrors() << ", warnings: " << D.getNumWarnings()
     << '\n';
  OS.flush();

  EXPECT_EQ(Out, (*Expected)->getBuffer().str());
}
