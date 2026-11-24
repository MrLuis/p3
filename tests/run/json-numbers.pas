{ Unit json, numbers: every number read by TJsonReader is the one strtod
  reads (as a double) and, for integers that fit, the exact Int64 / UInt64:
  digits of every length (the reader takes them 8 at a time), leading
  zeros of the fraction, exponents, signs, and the 19 / 20 digit limits.
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t | diff - %S/json-numbers.expected
  RUN: %p3 --mode=objfpc -O2 %s -o %t.fast
  RUN: %t.fast | diff - %S/json-numbers.expected
}
program jsonnumbers;
{$H+}
uses sysutils, json;

function c_strtod(p: PChar; endp: Pointer): Double; cdecl; external 'c' name 'strtod';

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

function Digits(N: Integer): string;
var
  I: Integer;
begin
  SetLength(Result, N);
  for I := 1 to N do
    Result[I] := Chr(Ord('0') + Integer(Next mod 10));
end;

procedure Check(const Text: string);
var
  R: TJsonReader;
  Want, Got: Double;
begin
  Inc(Checked);
  Want := c_strtod(PChar(Text), nil);
  R := TJsonReader.Create(Text);
  try
    R.Read;
    Got := R.AsDouble;
  finally
    R.Free;
  end;
  if Got <> Want then
  begin
    Inc(Bad);
    if Bad <= 10 then
      Writeln('differs: ', Text);
  end;
end;

procedure CheckInt(const Text: string; Want: Int64);
var
  R: TJsonReader;
begin
  Inc(Checked);
  R := TJsonReader.Create(Text);
  try
    R.Read;
    if R.AsInt64 <> Want then
    begin
      Inc(Bad);
      Writeln('int differs: ', Text);
    end;
  finally
    R.Free;
  end;
end;

var
  I, J, K: Integer;
  S, T: string;
begin
  Checked := 0;
  Bad := 0;
  { integers of 1 to 30 digits, with and without a fraction and exponent }
  for I := 1 to 30 do
    for J := 1 to 200 do
    begin
      S := Chr(Ord('1') + Integer(Next mod 9)) + Digits(I - 1);
      Check(S);
      Check('-' + S);
      Check(S + '.' + Digits(1 + Integer(Next mod 25)));
      Check(S + 'e' + IntToStr(Integer(Next mod 40) - 20));
      Check(S + '.' + Digits(1 + Integer(Next mod 25)) + 'E+' + IntToStr(Integer(Next mod 30)));
    end;
  { fractions: leading zeros then digits }
  for I := 0 to 25 do
    for J := 1 to 100 do
    begin
      T := '';
      for K := 1 to I do
        T := T + '0';
      Check('0.' + T + Chr(Ord('1') + Integer(Next mod 9)) + Digits(Integer(Next mod 24)));
      Check('12.' + T + Chr(Ord('1') + Integer(Next mod 9)) + Digits(Integer(Next mod 24)));
      Check('0.' + T + '0');
    end;
  { the exact integers }
  CheckInt('0', 0);
  CheckInt('12345678', 12345678);
  CheckInt('123456789', 123456789);
  CheckInt('1234567890123456', 1234567890123456);
  CheckInt('9007199254740993', 9007199254740993);
  CheckInt('9223372036854775807', 9223372036854775807);
  CheckInt('-9223372036854775808', -9223372036854775807 - 1);
  CheckInt('-12345678', -12345678);
  Check('18446744073709551615');
  Check('18446744073709551616');
  Check('99999999999999999999');
  Check('0.99999999999999999999');
  Check('1.00000000000000000000001');
  Check('123456789.123456789');
  Check('8.8888888');
  Check('88888888');
  Check('88888888.88888888');
  Writeln('checked ', Checked, ', differences ', Bad);
end.
