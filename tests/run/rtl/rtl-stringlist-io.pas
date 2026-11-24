{ TStringList.LoadFromFile and DelimitedText: line endings (LF, CR, CRLF,
  mixed, a CR at the end of a 64 KB block with its LF in the next), no final
  line break, empty lines, empty file, very long lines, and the quoting of
  items by DelimitedText (strict or not, with quotes inside, empty items).
  RUN: rm -rf %t && mkdir -p %t && %p3 --mode=objfpc %s -o %t/a
  RUN: cd %t && ./a > out && diff %S/rtl-stringlist-io.expected out
  RUN: %p3 --mode=objfpc -O2 %s -o %t/a2
  RUN: cd %t && ./a2 > out2 && diff %S/rtl-stringlist-io.expected out2
}
program stringlistio;
{$H+}{$Q-}{$R-}
uses sysutils, classes;

procedure Put(const Text: string);
var
  F: file;
begin
  Assign(F, 'in.tmp');
  Rewrite(F, 1);
  if Length(Text) > 0 then
    BlockWrite(F, Text[1], Length(Text));
  Close(F);
end;

function Sum(L: TStringList): string;
var
  I, H: Integer;
  S: string;
begin
  H := 0;
  for I := 0 to L.Count - 1 do
  begin
    S := L[I];
    H := (H * 31 + Length(S) + 7 * I) and $FFFFFF;
    if Length(S) > 0 then
      H := (H * 31 + Ord(S[1]) + Ord(S[Length(S)])) and $FFFFFF;
  end;
  Result := IntToStr(L.Count) + ' lines, check ' + IntToStr(H);
end;

procedure Load(const What, Text: string);
var
  L: TStringList;
begin
  Put(Text);
  L := TStringList.Create;
  L.LoadFromFile('in.tmp');
  Write(What, ': ', Sum(L));
  if (L.Count > 0) and (L.Count < 6) and (Length(L.Text) < 100) then
    Write(' [', StringReplace(L.Text, #13#10, '|', [rfReplaceAll]), ']');
  Writeln;
  L.Free;
end;

function Filler(N: Integer; C: Char): string;
begin
  SetLength(Result, N);
  FillChar(Result[1], N, C);
end;

procedure Delim(const What: string; const Items: array of string; Strict: Boolean);
var
  L: TStringList;
  I: Integer;
begin
  L := TStringList.Create;
  for I := 0 to High(Items) do
    L.Add(Items[I]);
  L.StrictDelimiter := Strict;
  Writeln(What, ': <', L.DelimitedText, '>');
  L.Free;
end;

begin
  Load('empty', '');
  Load('one line, no break', 'abc');
  Load('one line, LF', 'abc' + #10);
  Load('LF', 'a' + #10 + 'bb' + #10 + 'ccc' + #10);
  Load('CR', 'a' + #13 + 'bb' + #13 + 'ccc' + #13);
  Load('CRLF', 'a' + #13#10 + 'bb' + #13#10 + 'ccc' + #13#10);
  Load('mixed', 'a' + #13#10 + 'b' + #10 + 'c' + #13 + 'd' + #10#13 + 'e');
  Load('empty lines', #10#10 + 'x' + #13#13 + #13#10#13#10 + 'y');
  Load('no final break', 'a' + #10 + 'b');
  Load('only breaks', #13#10#13#10#10);
  { a CR as the last byte of the first 64 KB block, its LF at the start of the next }
  Load('CR at the block end', Filler(65535, 'x') + #13 + #10 + 'next' + #10 + 'last');
  Load('CRLF around the block end', Filler(65534, 'y') + #13#10 + 'next' + #13#10);
  Load('LF at the block end', Filler(65535, 'z') + #10 + 'next');
  Load('long line over blocks', Filler(200000, 'w') + #10 + 'end');
  Load('long line, no break', Filler(150000, 'v'));
  Load('lines over blocks', Filler(70000, 'a') + #13#10 + Filler(70000, 'b') + #10 + Filler(5, 'c') + #13);
  Delim('plain', ['a', 'b', 'c'], False);
  Delim('spaces', ['a b', 'c', ' d'], False);
  Delim('spaces strict', ['a b', 'c', ' d'], True);
  Delim('commas', ['a,b', 'c'], True);
  Delim('quotes', ['say "hi"', 'x', '"'], False);
  Delim('empty items', ['', '', ''], False);
  Delim('one empty', [''], False);
  Delim('none', [], False);
  Delim('one', ['solo'], False);
  Delim('nul and control', ['a' + #1 + 'b', 'c' + #9], False);
end.
