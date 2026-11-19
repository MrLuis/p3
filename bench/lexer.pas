{ A lexer: character-set tests, substrings and keyword lookup over a
  source text built in memory. }
program lexer;
{$H+}
type
  TKind = (kIdent, kKeyword, kNumber, kString, kSymbol, kComment);

const
  Snippets: array[0..7] of ansistring = (
    'program demo; var count, total: integer; ',
    'begin count := 0; total := 42 * (count + 7); ',
    'if total > 100 then writeln(''big'') else writeln(''small''); ',
    '{ a comment } while count < 10 do begin count := count + 1 end; ',
    'for i := 1 to 99 do total := total div 3 + i mod 5; ',
    'case total of 1: x := 2; 3..9: y := x; else z := 0 end; ',
    'repeat total := total - 1 until total <= 0; ',
    'procedure p(a: string; var b: real); begin b := 3.25 end; end.');

var
  counts: array[TKind] of longint;
  keywordHash: longint;

{ Keywords: a case on the first character, then string comparisons (p3
  has no case on strings). }
function KeywordCode(const s: ansistring): longint;
begin
  Result := 0;
  case s[1] of
    'b': if s = 'begin' then Result := 3;
    'c': if s = 'case' then Result := 12;
    'd': if s = 'do' then Result := 9
         else if s = 'div' then Result := 17;
    'e': if s = 'end' then Result := 4
         else if s = 'else' then Result := 7;
    'f': if s = 'for' then Result := 10;
    'i': if s = 'if' then Result := 5;
    'm': if s = 'mod' then Result := 18;
    'o': if s = 'of' then Result := 13;
    'p': if s = 'program' then Result := 1
         else if s = 'procedure' then Result := 16;
    'r': if s = 'repeat' then Result := 14;
    't': if s = 'then' then Result := 6
         else if s = 'to' then Result := 11;
    'u': if s = 'until' then Result := 15;
    'v': if s = 'var' then Result := 2;
    'w': if s = 'while' then Result := 8;
  end;
end;

procedure Scan(const text: ansistring);
var
  i, n, start, code: longint;
  c: char;
  word: ansistring;
begin
  i := 1;
  n := length(text);
  while i <= n do
  begin
    c := text[i];
    if c in [' ', #9, #10, #13] then
      i := i + 1
    else if c in ['a'..'z', 'A'..'Z', '_'] then
    begin
      start := i;
      while (i <= n) and (text[i] in ['a'..'z', 'A'..'Z', '0'..'9', '_']) do
        i := i + 1;
      word := Copy(text, start, i - start);
      code := KeywordCode(word);
      if code > 0 then
      begin
        counts[kKeyword] := counts[kKeyword] + 1;
        keywordHash := (keywordHash * 31 + code) mod 1000003;
      end
      else
        counts[kIdent] := counts[kIdent] + 1;
    end
    else if c in ['0'..'9'] then
    begin
      while (i <= n) and (text[i] in ['0'..'9', '.']) do
        i := i + 1;
      counts[kNumber] := counts[kNumber] + 1;
    end
    else if c = '''' then
    begin
      i := i + 1;
      while (i <= n) and (text[i] <> '''') do
        i := i + 1;
      i := i + 1;
      counts[kString] := counts[kString] + 1;
    end
    else if c = '{' then
    begin
      while (i <= n) and (text[i] <> '}') do
        i := i + 1;
      i := i + 1;
      counts[kComment] := counts[kComment] + 1;
    end
    else
    begin
      if (i < n) and (c in [':', '<', '>', '.']) and (text[i + 1] in ['=', '>', '.']) then
        i := i + 2
      else
        i := i + 1;
      counts[kSymbol] := counts[kSymbol] + 1;
    end;
  end;
end;

var
  text: ansistring;
  i: longint;
  k: TKind;
begin
  text := '';
  for i := 0 to 399 do
    text := text + Snippets[(i * 5) mod 8];
  keywordHash := 0;
  for i := 1 to 1500 do
    Scan(text);
  for k := low(TKind) to high(TKind) do
    write(counts[k], ' ');
  writeln;
  writeln(keywordHash);
end.
