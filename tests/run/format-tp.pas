{ Format and array of const in --mode=tp; a bad format is a runtime error,
  there being no exceptions to catch.
  RUN: %pcc --mode=tp %s -o %t
  RUN: not %t 2>&1 | FileCheck %s
  CHECK: 3 x 1.50 2
  CHECK-NEXT: Runtime error: unhandled exception EConvertError: Invalid argument index in format "%d"
}
program formattp;
uses sysutils;
function Count(const a: array of const): longint;
begin
  Count := high(a) + 1;
end;
begin
  writeln(Format('%d %s %.2f', [3, 'x', 1.5]), ' ', Count([1, 'a']));
  writeln(Format('%d', ['bad']));
end.
