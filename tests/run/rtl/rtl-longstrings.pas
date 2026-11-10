{ pcc's unit library on strings longer than 255 characters. Output identical
  to Free Pascal 3.2.2's (fpc -Mobjfpc). Run in its own directory: it creates
  and removes files.
  RUN: rm -rf %t && mkdir -p %t && %pcc --mode=objfpc %s -o %t/a
  RUN: cd %t && ./a > out && diff %S/rtl-longstrings.expected out
  RUN: %pcc --mode=objfpc -fseparate-units %s -o %t/a2
  RUN: cd %t && ./a2 > out2 && diff %S/rtl-longstrings.expected out2
}
program rtllongstrings;
{$H+}
uses sysutils, strutils, classes;
function c_fopen(n, m: pchar): pointer; cdecl; external 'c' name 'fopen';
function c_fputc(c: longint; f: pointer): longint; cdecl; external 'c' name 'fputc';
function c_fclose(f: pointer): longint; cdecl; external 'c' name 'fclose';
var
  f: pointer;
  i: longint;
  a, b: string;
  s, t: TStringList;
  sum: longint;
function Check(const x: string): longint;
var i, h: longint;
begin
  h := 0;
  for i := 1 to length(x) do
    h := (h * 31 + ord(x[i])) mod 1000003;
  Check := h;
end;
begin
  a := DupeString('abc ', 200);
  writeln(length(a), ' ', Check(a));
  b := UpperCase(a); writeln(length(b), ' ', Check(b), ' ', Check(LowerCase(b)));
  writeln(length(Trim('  ' + a + '  ')), ' ', length(TrimLeft(a)), ' ', length(TrimRight(a)));
  b := StringReplace(a, 'bc', 'XYZ', [rfReplaceAll]); writeln(length(b), ' ', Check(b));
  b := StringReplace(a, 'BC', '', [rfReplaceAll, rfIgnoreCase]); writeln(length(b), ' ', Check(b));
  writeln(length(QuotedStr(a)), ' ', SameText(a, UpperCase(a)), ' ', CompareStr(a, a), ' ', LastDelimiter('a', a));
  writeln(length(ReverseString(a)), ' ', Check(ReverseString(a)));
  writeln(length(LeftStr(a, 500)), ' ', length(RightStr(a, 500)), ' ', length(MidStr(a, 300, 400)));
  writeln(length(PadLeft('x', 400)), ' ', length(PadRight('x', 400)), ' ', length(AddChar('0', 'x', 300)));
  writeln(PosEx('c', a, 700), ' ', length(ReplaceStr(a, 'a', 'aa')), ' ', length(StuffString(a, 300, 10, 'zz')));
  writeln(StartsStr(LeftStr(a, 300), a), EndsStr(RightStr(a, 300), a), ContainsStr(a, 'c a'));
  writeln(length(ExtractFileName(DupeString('d/', 150) + 'name.txt')), ' ', length(ExtractFilePath(DupeString('d/', 150) + 'name.txt')), ' ', length(ChangeFileExt(DupeString('e', 300) + '.a', '.bcd')));
  writeln(length(IntToStr(5) + a), ' ', ExtractFileExt(a + '.long'));
  f := c_fopen('long.txt', 'wb');
  for i := 1 to 1000 do c_fputc(ord('x'), f);
  c_fputc(10, f);
  for i := 1 to 5000 do c_fputc(48 + i mod 10, f);
  c_fputc(13, f); c_fputc(10, f);
  c_fputc(ord('y'), f);
  c_fclose(f);
  s := TStringList.Create;
  s.LoadFromFile('long.txt');
  writeln(s.Count, ' ', length(s[0]), ' ', length(s[1]), ' ', Check(s[1]), ' ', s[2]);
  s.SaveToFile('long2.txt');
  t := TStringList.Create;
  t.LoadFromFile('long2.txt');
  writeln(t.Equals(s), ' ', t.Count, ' ', length(t[1]));
  DeleteFile('long.txt'); DeleteFile('long2.txt');
  s.Clear;
  for i := 1 to 100 do s.Add('line' + IntToStr(i));
  writeln(length(s.Text), ' ', Check(s.Text));
  writeln(length(s.CommaText), ' ', Check(s.CommaText));
  s.Delimiter := ';'; writeln(length(s.DelimitedText), ' ', Check(s.DelimitedText));
  t.Text := s.Text; writeln(t.Count, ' ', t[99], ' ', t.Equals(s));
  t.CommaText := s.CommaText; writeln(t.Count, ' ', t[0], ' ', t.Equals(s));
  t.Clear; t.Add(a); t.Add(a + a); t.Sort; writeln(t.Count, ' ', length(t[0]), ' ', length(t[1]), ' ', t.IndexOf(a));
  t.Values[DupeString('k', 300)] := DupeString('v', 300); writeln(length(t[2]), ' ', length(t.Values[DupeString('k', 300)]), ' ', length(t.Names[2]));
  t[0] := a + 'changed'; writeln(length(t[0]), ' ', length(a));
  sum := 0; for i := 0 to t.Count - 1 do sum := sum + length(t[i]); writeln(sum);
  s.Free; t.Free;
  writeln(length(FloatToStr(1.5)), IntToStr(12345), ' ', StrToInt('  42'), ' ', StrToFloat('2.5'):0:1);
  writeln('done');
end.
