{ Two declarations of one C routine with different results (a procedure
  and a function of C's abs): each call uses its own declaration's type.
  (netasync declares closesocket as a procedure, netloop as a function;
  the self-hosted compiler called both with the first one's.)
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t | diff - %S/external-alias.expected
  RUN: %p3 --mode=objfpc -O2 %s -o %t.O2
  RUN: %t.O2 | diff - %S/external-alias.expected
}
program externalalias;

procedure AbsIgnored(x: LongInt); cdecl; external 'c' name 'abs';
function AbsOf(x: LongInt): LongInt; cdecl; external 'c' name 'abs';

begin
  AbsIgnored(-3);
  Writeln(AbsOf(-5), ' ', AbsOf(7));
end.
