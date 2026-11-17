{ The rtlproc directive (not in --mode=iso), as in Free Pascal: marks a
  routine of Free Pascal's run-time library and has no effect; with other
  directives, forward, on methods; not reserved. Output is identical to
  Free Pascal 3.2.2's (fpc -Mobjfpc).
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t | diff - %S/rtlproc.expected
  RUN: %p3 --mode=tp %s -o %t.tp
  RUN: %t.tp | diff - %S/rtlproc.expected
}
program rtlproc_directive;
{$inline on}
type PathStr = string[255];
Procedure DoDirSeparators (Var FileName : PathStr); rtlproc;
var i: integer;
begin
  for i := 1 to length(FileName) do if FileName[i] = '\' then FileName[i] := '/';
end;
procedure Fwd(x: integer); rtlproc; forward;
procedure Fwd(x: integer); begin writeln(x) end;
function F(x: integer): integer; rtlproc; inline; begin F := x * 2 end;
procedure C(x: integer); cdecl; rtlproc; begin writeln('c', x) end;
type TP = procedure(var s: PathStr);
var s: PathStr; p: TP; rtlproc: integer;
begin
  s := 'a\b\c'; DoDirSeparators(s); writeln(s);
  Fwd(3); writeln(F(4)); C(5);
  p := @DoDirSeparators; s := 'x\y'; p(s); writeln(s);
  rtlproc := 6; writeln(rtlproc);
end.
