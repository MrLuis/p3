{ pcc's unit library in --mode=tp: no overloads and no exceptions, so the
  one-signature routines only, and a bad conversion is a runtime error.
  RUN: %pcc --mode=tp %s -o %t
  RUN: not %t 2>&1 | FileCheck %s
  CHECK: 42 FF hello 3 8
  CHECK-NEXT: 1.5
  CHECK-NEXT: 7
  CHECK-NEXT: Runtime error: "zz" is an invalid integer
}
program rtltp;
uses sysutils, strutils, math;
begin
  writeln(IntToStr(42), ' ', IntToHex(255, 1), ' ', LowerCase('HELLO'), ' ', Floor(3.5), ' ', Max(int64(3), int64(8)));
  writeln(FloatToStr(1.5));
  writeln(StrToIntDef('zz', 7));
  writeln(StrToInt('zz'));
end.
