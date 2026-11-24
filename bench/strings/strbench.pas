program strbench;
{ The string routines of the unit library that scan bytes, on short and
  long inputs: ns per call (best of several runs). Compare two builds of
  the units: run.sh (the current ones against those of another commit).

    strbench [SECONDS] }
{$H+}
uses sysutils, strutils, classes, nethttp;

function c_clock: Int64; cdecl; external 'c' name 'clock';

type
  TOp = procedure;

var
  Budget: Double = 0.3;
  Sink: Int64;
  Short, Mid, Long, LongUpper, Needle, Head, Lines: string;
  A64, B64, A1K, B1K: string;
  Ctx: THttpContext;

function Now: Double;
begin
  Result := c_clock / 1000000;
end;

procedure Measure(const Name: string; Op: TOp; Reps: Integer);
var
  Start, T, Best: Double;
  Runs, I: Integer;
begin
  Best := 1e9;
  Runs := 0;
  Start := Now;
  repeat
    T := Now;
    for I := 1 to Reps do
      Op();
    T := (Now - T) / Reps;
    if T < Best then
      Best := T;
    Inc(Runs);
  until (Now - Start > Budget) and (Runs >= 3);
  WriteLn(Format('  %-28s %10.1f ns', [Name, Best * 1e9]));
end;

procedure PosShort; begin Inc(Sink, PosEx('fox', Short, 1)); end;
procedure PosMid; begin Inc(Sink, PosEx('lazy dog', Mid, 1)); end;
procedure PosLong; begin Inc(Sink, PosEx(Needle, Long, 1)); end;
procedure ReplaceShort; begin Inc(Sink, Length(StringReplace(Short, 'o', '0', [rfReplaceAll]))); end;
procedure ReplaceLong; begin Inc(Sink, Length(StringReplace(Long, 'needle', 'pin', [rfReplaceAll]))); end;
procedure CmpShort; begin Inc(Sink, CompareStr('content-length', 'content-type')); end;
procedure Cmp64; begin Inc(Sink, CompareStr(A64, B64)); end;
procedure Cmp1K; begin Inc(Sink, CompareStr(A1K, B1K)); end;
procedure TextShort; begin Inc(Sink, CompareText('Content-Length', 'content-length')); end;
procedure Text1K; begin Inc(Sink, CompareText(A1K, B1K)); end;
procedure UpperShort; begin Inc(Sink, Length(UpperCase('content-type'))); end;
procedure UpperLong; begin Inc(Sink, Length(UpperCase(Long))); end;
procedure UpperNone; begin Inc(Sink, Length(UpperCase(LongUpper))); end;
procedure LowerLong; begin Inc(Sink, Length(LowerCase(LongUpper))); end;
procedure SetText;
var
  L: TStringList;
begin
  L := TStringList.Create;
  L.Text := Lines;
  Inc(Sink, L.Count);
  L.Free;
end;
procedure ParseHead;
var
  Used: Integer;
begin
  if HttpParseRequest(PAnsiChar(Head), Length(Head), Ctx, Used) <> hpOk then
    Halt(3); { (the request must parse: a benchmark of a failure is none) }
  Inc(Sink, Used);
end;

var
  I: Integer;
begin
  if ParamCount > 0 then
    Budget := StrToFloat(ParamStr(1));
  Short := 'the quick brown fox';
  Mid := 'The quick brown fox jumps over the lazy dog, again and again.';
  Long := '';
  for I := 1 to 2000 do
    Long := Long + 'Lorem ipsum dolor sit amet, consectetur adipiscing elit ' + IntToStr(I) + '. ';
  Long := Long + 'needle';
  Needle := 'needle';
  LongUpper := UpperCase(Long);
  A64 := '';
  for I := 1 to 64 do
    A64 := A64 + Chr(Ord('a') + I mod 26);
  B64 := A64;
  B64[60] := 'Z';
  A1K := '';
  for I := 1 to 1024 do
    A1K := A1K + Chr(Ord('a') + I mod 26);
  B1K := A1K;
  B1K[1000] := 'Z';
  Lines := '';
  for I := 1 to 2000 do
    Lines := Lines + 'line number ' + IntToStr(I) + ' of the text, with some words' + #10;
  { (line by line: a constant expression longer than 255 bytes is cut) }
  Head := 'GET /api/v1/users/12345?fields=name,email HTTP/1.1'#13#10;
  Head := Head + 'Host: example.com'#13#10;
  Head := Head + 'User-Agent: Mozilla/5.0 (Macintosh; Intel Mac OS X 14_0) AppleWebKit/537.36'#13#10;
  Head := Head + 'Accept: text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8'#13#10;
  Head := Head + 'Accept-Language: en-US,en;q=0.5'#13#10;
  Head := Head + 'Accept-Encoding: gzip, deflate, br'#13#10;
  Head := Head + 'Connection: keep-alive'#13#10;
  Head := Head + 'Cookie: session=abcdef0123456789abcdef0123456789; theme=dark'#13#10#13#10;
  Ctx := THttpContext.Create;
  WriteLn('strings (', Length(Long), ' bytes long)');
  Measure('PosEx short (19 B)', @PosShort, 20000);
  Measure('PosEx mid (61 B)', @PosMid, 20000);
  Measure('PosEx long', @PosLong, 20);
  Measure('StringReplace short', @ReplaceShort, 5000);
  Measure('StringReplace long', @ReplaceLong, 20);
  Measure('CompareStr short', @CmpShort, 20000);
  Measure('CompareStr 64 B', @Cmp64, 20000);
  Measure('CompareStr 1 KB', @Cmp1K, 5000);
  Measure('CompareText short', @TextShort, 20000);
  Measure('CompareText 1 KB', @Text1K, 5000);
  Measure('UpperCase short', @UpperShort, 20000);
  Measure('UpperCase long', @UpperLong, 20);
  Measure('UpperCase long, none', @UpperNone, 20);
  Measure('LowerCase long', @LowerLong, 20);
  Measure('TStringList.Text (2000 lines)', @SetText, 5);
  Measure('HttpParseRequest (8 headers)', @ParseHead, 5000);
  if Sink = 42 then
    WriteLn;
end.
