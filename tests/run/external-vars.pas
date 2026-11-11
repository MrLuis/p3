{ External (C) variables (not in --mode=iso), as in Free Pascal:
  `x: T; external ['lib'] [name 'sym'];` and `x: T; cvar; external;` (the
  symbol: the name as written unless given; `external` and `cvar` are not
  reserved). errno is the C library's per-thread one, through its accessor;
  StdCLib (classic Mac OS) names the C library.
  RUN: %pcc --mode=objfpc %s -o %t
  RUN: %t | FileCheck %s
  RUN: %pcc --mode=tp -O2 %s -o %t.O2
  RUN: %t.O2 | FileCheck %s
  CHECK:      1 1 1
  CHECK-NEXT: 5 3
  CHECK-NEXT: -1 9
  CHECK-NEXT: 0
}
program externalvars;
type
  C_int = longint;
const
  CLib = 'c';
var
  opterr1: C_int; external 'c' name 'opterr';
  optind: C_int; cvar; external;
  opterr2: C_int; external CLib name 'opterr';
  errno: C_int; external 'StdCLib' name 'errno';
  external: integer;

function close(fd: C_int): C_int; cdecl; external 'c';

begin
  writeln(opterr1, ' ', optind, ' ', opterr2);
  opterr1 := 5;
  external := 3;
  writeln(opterr2, ' ', external);
  writeln(close(99), ' ', errno);
  errno := 0;
  writeln(errno);
end.
