{ Unit json, JsonFloat: the same text as "%.15g", "%.16g", "%.17g" (the
  shortest that reads back, plus ".0") for the doubles of every kind:
  integers, short decimals, powers of ten, subnormals, the extremes, and
  200000 random bit patterns, each compared with that definition written here.
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t | diff - %S/json-float.expected
  RUN: %p3 --mode=objfpc -O2 %s -o %t.fast
  RUN: %t.fast | diff - %S/json-float.expected
}
program jsonfloat;
{$H+}
uses sysutils, json;

function c_snprintf(buf: PChar; n: SizeInt; fmt: PChar): Integer; cdecl;
  varargs; external 'c' name 'snprintf';
function c_strtod(p: PChar; endp: Pointer): Double; cdecl; external 'c' name 'strtod';

{ the definition }
function Reference(D: Double): string;
var
  Buf: array[0..39] of Char;
  P: Integer;
  S: string;
begin
  for P := 15 to 17 do
  begin
    c_snprintf(@Buf[0], 40, '%.*g', P, D);
    if (P = 17) or (c_strtod(@Buf[0], nil) = D) then
      Break;
  end;
  S := PChar(@Buf[0]);
  { Windows' C library writes at least three exponent digits; the definition
    has at least two, like C99 }
  P := Pos('e', S);
  if P > 0 then
    while (Length(S) - P > 3) and (S[P + 2] = '0') do
      Delete(S, P + 2, 1);
  if (Pos('.', S) = 0) and (Pos('e', S) = 0) then
    S := S + '.0';
  Result := S;
end;

type
  PDouble = ^Double;

var
  Seed: QWord = 88172645463325252;
  Checked, Bad: Integer;

function Next: QWord;
begin
  Seed := Seed xor (Seed shl 13);
  Seed := Seed xor (Seed shr 7);
  Seed := Seed xor (Seed shl 17);
  Result := Seed;
end;

procedure Check(D: Double);
begin
  Inc(Checked);
  if JsonFloat(D) <> Reference(D) then
  begin
    Inc(Bad);
    if Bad <= 10 then
      Writeln('differs: ', Reference(D), ' / ', JsonFloat(D));
  end;
end;

var
  I, J: Integer;
  Bits: QWord;
  D: Double;
begin
  Checked := 0;
  Bad := 0;
  { named values }
  Writeln(JsonFloat(0.0), ' ', JsonFloat(-0.0), ' ', JsonFloat(1.0), ' ', JsonFloat(-2.5), ' ',
          JsonFloat(0.1), ' ', JsonFloat(1e21), ' ', JsonFloat(1e-7), ' ', JsonFloat(123456789012345680000.0),
          ' ', JsonFloat(0.30000000000000004), ' ', JsonFloat(5e-324), ' ', JsonFloat(1.7976931348623157e308));
  Check(0.0); Check(-0.0); Check(5e-324); Check(2.2250738585072014e-308);
  Check(1.7976931348623157e308); Check(-1.7976931348623157e308);
  Check(0.1); Check(0.2); Check(0.3); Check(1.0 / 3.0); Check(2.0 / 3.0);
  Check(9007199254740992.0); Check(9007199254740993.0); Check(1e15); Check(1e16); Check(1e17);
  Check(123456789012345678.0); Check(999999999999999.9); Check(0.9999999999999999);
  { integers and short decimals }
  for I := -3000 to 3000 do
  begin
    Check(I);
    Check(I / 8);
    Check(I / 10);
    Check(I / 100);
    Check(I * 1000003.0);
    Check(I / 7);
  end;
  { powers of ten and neighbours }
  D := 1e-320;
  for I := 1 to 640 do
  begin
    Check(D);
    Check(D * 1.0000000000000002);
    Check(D * 0.9999999999999999);
    D := D * 10;
    if D - D <> 0 then
      Break;
  end;
  { random bit patterns (finite ones) }
  for I := 1 to 200000 do
  begin
    Bits := Next;
    D := PDouble(@Bits)^;
    if D - D = 0 then
      Check(D);
  end;
  { random decimals with 1 to 17 digits }
  for I := 1 to 100000 do
  begin
    Bits := Next mod 100000000000000000;
    J := 1 + Integer(Next mod 17);
    while J > 0 do
    begin
      Bits := Bits div 10;
      Dec(J);
      if Bits < 10 then
        Break;
    end;
    D := Bits;
    J := Integer(Next mod 40) - 25;
    while J > 0 do begin D := D * 10; Dec(J); end;
    while J < 0 do begin D := D / 10; Inc(J); end;
    Check(D);
    Check(-D);
  end;
  Writeln('checked ', Checked, ', differences ', Bad);
end.
