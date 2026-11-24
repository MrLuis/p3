{ Unit json, escaped strings: AsString decodes them (an escape anywhere in
  a long string, at every offset around the 16-byte steps, \u escapes of 1
  to 4 UTF-8 bytes, surrogate pairs) and StringEquals compares an escaped
  string with a plain one without decoding: true for the decoded text, false
  for anything shorter, longer or different anywhere.
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t | diff - %S/json-escapes.expected
  RUN: %p3 --mode=objfpc -O2 %s -o %t.fast
  RUN: %t.fast | diff - %S/json-escapes.expected
}
program jsonescapes;
{$H+}
uses sysutils, json;

var
  Checked, Bad: Integer;

procedure Check(const Source, Want: string);
var
  R: TJsonReader;
  Got, Other: string;
  I: Integer;
begin
  R := TJsonReader.Create('"' + Source + '"');
  try
    R.Read;
    Got := R.AsString;
    Inc(Checked);
    if Got <> Want then
    begin
      Inc(Bad);
      Writeln('decoded differently: ', Source);
    end;
    if not R.StringEquals(Want) then
    begin
      Inc(Bad);
      Writeln('not equal to its decoding: ', Source);
    end;
    { one byte different, shorter, longer }
    for I := 1 to Length(Want) do
    begin
      Other := Want;
      Other[I] := Chr((Ord(Other[I]) + 1) and 255);
      if R.StringEquals(Other) then
      begin
        Inc(Bad);
        Writeln('equal to a changed copy: ', Source, ' at ', I);
      end;
    end;
    if Length(Want) > 0 then
      if R.StringEquals(Copy(Want, 1, Length(Want) - 1)) or R.StringEquals(Want + 'x') then
      begin
        Inc(Bad);
        Writeln('equal to a different length: ', Source);
      end;
  finally
    R.Free;
  end;
end;

function Filler(N: Integer): string;
var
  I: Integer;
begin
  SetLength(Result, N);
  for I := 1 to N do
    Result[I] := Chr(Ord('a') + I mod 26);
end;

var
  I, J: Integer;
  F: string;
begin
  Checked := 0;
  Bad := 0;
  Check('a\nb', 'a' + #10 + 'b');
  Check('\\', '\');
  Check('\"\/', '"/');
  Check('\b\f\r\t', #8#12#13#9);
  Check('A', 'A');
  Check('é', #$C3#$A9);
  Check('€', #$E2#$82#$AC);
  Check('😀', #$F0#$9F#$98#$80);
  Check('x\u0000y', 'x' + #0 + 'y');
  Check('\n', #10);
  { an escape at every offset of strings of 0 to 40 bytes }
  for I := 0 to 40 do
  begin
    F := Filler(I);
    for J := 0 to I do
    begin
      Check(Copy(F, 1, J) + '\n' + Copy(F, J + 1, I), Copy(F, 1, J) + #10 + Copy(F, J + 1, I));
      Check(Copy(F, 1, J) + 'é' + Copy(F, J + 1, I), Copy(F, 1, J) + #$C3#$A9 + Copy(F, J + 1, I));
    end;
  end;
  { long strings with escapes far apart }
  F := Filler(5000);
  Check(F + '\t' + F, F + #9 + F);
  Check(F + '\t' + F + '\\' + F + '€', F + #9 + F + '\' + F + #$E2#$82#$AC);
  Writeln('checked ', Checked, ', wrong ', Bad);
end.
