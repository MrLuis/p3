{ Macros ($macro on, $define NAME := value, as in Free Pascal): any
  tokens as a value, expanded where the name is used (case-insensitive,
  lazily: a macro used in another's value is expanded with its value at
  the time), $undef, redefinition, $macro off; macro values in $IF
  expressions (integers, booleans, arithmetic, comparisons, in [...]).
  Output is identical to Free Pascal 3.2.2's (fpc -Mobjfpc).
  RUN: %pcc --mode=objfpc %s -o %t
  RUN:  %t > %t.out
  RUN: diff %S/macros.expected %t.out
  RUN: %pcc --mode=objfpc -O2 %s -o %t.O2
  RUN:  %t.O2 > %t.O2.out
  RUN: diff %S/macros.expected %t.O2.out
}
program macros;

{$macro on}
{$define SIZE := 4}
{$define LAST := SIZE}
{$define DOUBLE := (SIZE * 2)}
{$define GREETING := 'hello'}
{$define SAY := writeln}
{$define LOG := SAY('log: ',}
{$define EMPTY :=}
{$define VERSION := 3}
{$define NEXT := VERSION}
{$define BITS := $10}

type
  TData = array[1..SIZE] of integer;

var
  data: TData;
  i: integer;

begin
  for i := 1 to SIZE do
    data[i] := i * DOUBLE;
  SAY(data[last], ' ', GREETING, ' ', Size + 1);
  LOG 'a message');
  EMPTY SAY('after an empty macro');

  {$if VERSION >= 3} SAY('version 3 or later'); {$endif}
  {$if (VERSION * 10 + 2) div 4 = 8} SAY('arithmetic'); {$endif}
  {$if VERSION mod 2 = 1} SAY('odd version'); {$endif}
  {$if 0 - VERSION < 0} SAY('negative'); {$endif}
  {$if (NEXT = 3) and (SIZE > 2)} SAY('a macro naming a macro'); {$endif}
  {$if not (VERSION = 2) or defined(SIZE)} SAY('defined size'); {$endif}
  {$if (BITS = 16) and ($10 = 16)} SAY('hex'); {$endif}
  {$if LAST = SIZE} SAY('same'); {$endif}
  {$if VERSION in [1, 3, 5]} SAY('in a set'); {$endif}
  {$if 7 in [1, 3, 5]} SAY('not reached'); {$else} SAY('not in the set'); {$endif}

  {$undef SIZE}
  {$ifndef SIZE} SAY('SIZE undefined'); {$endif}
  {$define SIZE := 100}
  SAY(DOUBLE, ' ', LAST);
  {$define SIZE}
  {$ifdef SIZE} SAY('SIZE defined again, without a value'); {$endif}
  {$macro off}
  writeln('macros off');
end.
