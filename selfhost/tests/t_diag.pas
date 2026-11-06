program t_diag;
{ pcdiag test. The same sequence of diagnostics is replayed through the
  C++ DiagnosticsEngine by tests/unit/SelfhostDiagTest.cpp, which checks
  that it prints exactly t_diag.expected: the Pascal port must match the
  C++ compiler byte for byte. Input: Inputs/diag.txt (tabs, a non-ASCII
  line, a CRLF line end, a 306-character line, no final newline). }

uses pcsys, pcutil, pcdiag;

var
  fid: longint;
  data: PCharArray;
  len: int64;

{ Location of the n-th (1-based) occurrence of s in the file. }
function find(const s: string; n: longint): TLoc;
var
  i, j: int64;
  ok: boolean;
begin
  {$R-}
  for i := 0 to len - length(s) do
  begin
    ok := true;
    for j := 1 to length(s) do
      if data^[i + j - 1] <> s[j] then
      begin
        ok := false;
        break;
      end;
    if ok then
    begin
      n := n - 1;
      if n = 0 then
        exit(smFileStart(fid) + TLoc(i));
    end;
  end;
  {$R+}
  sysWrite(sysOut, 'needle not found: ' + s + #10);
  sysExit(1);
  find := noLoc;
end;

procedure presumed(loc: TLoc);
var
  f, line, col: longint;
begin
  smPresumed(loc, f, line, col);
  sysWrite(sysOut, 'presumed ' + intToStr(line) + ':' + intToStr(col) + #10);
end;

var
  x, y, b, c, z, eofLoc: TLoc;
begin
  sysInit;
  diagInit;
  diagStream := sysOut;
  if not sysReadFile('Inputs/diag.txt', data, len) then
  begin
    sysWrite(sysOut, 'cannot read Inputs/diag.txt' + #10);
    sysExit(1);
  end;
  fid := smAddBuffer('diag.txt', data, len);

  x := find('x :=', 1);
  y := find('y;', 1);
  b := find('b +', 1);
  c := find('c;', 1);
  z := find('zzz', 1);
  eofLoc := smFileStart(fid) + TLoc(len);

  presumed(smFileStart(fid));
  presumed(y);
  presumed(c);
  presumed(eofLoc);

  { tabs, two ranges }
  diagErrorRR(y, 'use of undeclared identifier ''y''', makeRange(x, x + 1),
              makeRange(y, y + 1));
  { non-ASCII line: no caret line }
  diagWarningR(find('caf', 1), 'non-ASCII line', makeRange(find('caf', 1), find(':= 1', 1)));
  { a range continuing on the next line is clipped; CRLF line end }
  diagNoteR(b, 'range over two lines', makeRange(b, c + 1));
  { warning groups }
  diagWarningGroupR('unused-variable', find('a :=', 1),
                    'variable ''a'' is declared but never used',
                    makeRange(find('a :=', 1), find('a :=', 1) + 1));
  diagDisableWarning('unused-variable');
  diagWarningGroupR('unused-variable', c, 'disabled: not shown', noRange);
  { end of file, a long line, no location }
  diagError(eofLoc, 'unexpected end of file');
  diagErrorR(z + 290, 'far into a long line', makeRange(z, z + 300));
  diagError(noLoc, 'cannot open file ''nope.pas''');
  diagNote(c, 'note after an error');
  { -Werror }
  diagSetWarningsAsErrors(true);
  diagWarning(c, 'promoted to an error');
  diagSetWarningsAsErrors(false);
  { the error limit: one more error, then a fatal stop; later output is
    suppressed }
  diagSetErrorLimit(diagNumErrors + 1);
  diagError(x, 'last error');
  diagError(y, 'over the limit');
  diagNote(y, 'suppressed note');
  diagWarning(y, 'suppressed warning');
  sysWrite(sysOut, 'errors: ' + intToStr(diagNumErrors) + ', warnings: ' +
                   intToStr(diagNumWarnings) + #10);
  sysFlush;
end.
