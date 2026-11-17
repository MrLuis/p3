{ Initialized variables (--mode=objfpc):  var x: T = value;  Global ones
  start with their values; a routine's are set again on every call (also
  in recursion and nested routines), unlike typed constants, which keep
  their values. Ordinals, reals, chars, enumerations, subranges,
  strings, arrays and records. Output is identical to Free
  Pascal 3.2.2's (fpc -Mobjfpc).
  RUN: %p3 --mode=objfpc %s -o %t
  RUN:  %t > %t.out
  RUN: diff %S/initvars.expected %t.out
  RUN: %p3 --mode=objfpc -O2 %s -o %t.O2
  RUN:  %t.O2 > %t.O2.out
  RUN: diff %S/initvars.expected %t.O2.out
  RUN: %p3 --mode=objfpc -g %s -o %t.g
  RUN:  %t.g > %t.g.out
  RUN: diff %S/initvars.expected %t.g.out
}
program initvars;

type
  TColor = (red, green, blue);
  TDigit = 0..9;
  TPoint = record
    X, Y: integer;
    Name: string[8];
  end;

var
  flag: boolean = true;
  count: integer = 41;
  big: int64 = 10000000000;
  ratio: real = 2.5;
  letter: char = 'q';
  color: TColor = green;
  digit: TDigit = 7;
  greeting: string = 'hello';
  short: string[5] = 'abc';
  primes: array[1..4] of integer = (2, 3, 5, 7);
  origin: TPoint = (X: 1; Y: 2; Name: 'origin');
  untouched: integer;

procedure Counter(depth: integer);
var
  calls: integer = 100;
  seen: boolean = false;
  path: string = '>';
  local: TPoint = (X: 5; Y: 6; Name: 'local');
const
  total: integer = 0;      { a typed constant: keeps its value }

  procedure Inner;
  var
    k: integer = 3;
  begin
    k := k * 2;
    write(' inner ', k);
  end;

begin
  total := total + 1;
  write('  depth ', depth, ': ', calls, ' ', seen, ' ', path, ' ', local.X,
        local.Name, ' total ', total);
  Inner;
  writeln;
  calls := calls + 1;
  seen := true;
  path := path + '>';
  local.X := 0;
  local.Name := 'changed';
  if depth < 2 then
    Counter(depth + 1);
  writeln('  back at ', depth, ': ', calls, ' ', seen, ' ', path, ' ', local.X,
          local.Name);
end;

function Fresh: integer;
var
  acc: integer = 10;
begin
  acc := acc + 1;
  Fresh := acc;
end;

begin
  writeln(flag, ' ', count, ' ', big, ' ', ratio:0:2, ' ', letter, ' ',
          ord(color), ' ', digit);
  writeln(greeting, ' ', short, ' ', primes[3], ' ',
          origin.X, origin.Y, ' ', origin.Name, ' ', untouched);
  count := count + 1;
  greeting := 'bye';
  writeln(count, ' ', greeting);
  Counter(0);
  Counter(0);
  writeln(Fresh, ' ', Fresh);
end.
