{ The PUSH and POP directives (tested in tp and objfpc), as in Free
  Pascal: POP restores the local switches PUSH saved, nested:
  long strings (H), record alignment (PACKRECORDS, ALIGN, A), scoped
  enumerations, and range and overflow checks (R, Q). A PUSH or POP in a
  branch that is not compiled has no effect. Output is identical to Free
  Pascal 3.2.2's (fpc -Mobjfpc and -Mtp).
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t | diff - %S/push-pop.expected
  RUN: %p3 --mode=tp %s -o %t.tp
  RUN: %t.tp | diff - %S/push-pop.expected
}
program pushpop;
{$R-}
{$Q-}

type
{$push}
{$H+}
{$PACKRECORDS 1}
{$scopedenums on}
  TRec1 = record a: byte; b: longint; end;
  TEnum1 = (e1a, e1b);
  TStr1 = string;
{$push}
{$PACKRECORDS 2}
  TRec2 = record a: byte; b: longint; end;
{$pop}
  TRec3 = record a: byte; b: longint; end;
{$pop}
  TRec4 = record a: byte; b: longint; end;
  TEnum2 = (e2a, e2b);
  TStr2 = string;
{$ifdef NEVER}
{$push}
{$A1}
{$endif}
  TRec5 = record a: byte; b: longint; end;
{$push}{$A2}{$push}{$A1}{$pop}
  TRec6 = record a: byte; b: longint; end;
{$pop}
  TRec7 = record a: byte; b: longint; end;

var
  b: byte;
  i: integer;
  q: int64;

begin
  writeln(SizeOf(TRec1), ' ', SizeOf(TRec2), ' ', SizeOf(TRec3), ' ', SizeOf(TRec4),
    ' ', SizeOf(TRec5), ' ', SizeOf(TRec6), ' ', SizeOf(TRec7));
  writeln(SizeOf(TStr1), ' ', SizeOf(TStr2), ' ', Ord(TEnum1.e1b), ' ', Ord(e2b));
  i := 300;
  {$push}{$R+}{$pop}
  b := i; { range checks off again }
  writeln(b);
  {$push}{$Q+}{$pop}
  q := 9223372036854775807;
  q := q + 1; { overflow checks off again }
  writeln(q);
end.
