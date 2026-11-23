{ Unit json, the reader: tokens, strings, numbers, and what is rejected
  (with positions). Cases after JSONTestSuite (y_ accepted, n_ rejected).
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t | diff - %S/json-reader.expected
  RUN: %p3 --mode=objfpc -O2 %s -o %t.fast
  RUN: %t.fast | diff - %S/json-reader.expected
}
program jsonreader;
{$H+}
uses sysutils, json;

const
  TokenNames: array[TJsonToken] of string = ('none', '{', '}', '[', ']',
    'key', 'string', 'number', 'true', 'false', 'null', 'end');

procedure Tokens(const S: string);
var
  R: TJsonReader;
  Line: string;
begin
  R := TJsonReader.Create(S);
  Line := '';
  try
    while R.Read do
    begin
      Line := Line + TokenNames[R.Token];
      case R.Token of
        jtKey, jtString: Line := Line + '(' + R.AsString + ')';
        jtNumber: Line := Line + '(' + R.RawText + ')';
      end;
      Line := Line + ' ';
    end;
    WriteLn(Line);
  except
    on E: EJsonError do
      WriteLn(Line, '! ', E.Message);
  end;
  R.Free;
end;

{ accepted or not, and why }
procedure Check(const Name, S: string);
var
  R: TJsonReader;
begin
  R := TJsonReader.Create(S);
  try
    while R.Read do
      if R.Token in [jtString, jtKey] then
        R.AsString
      else if R.Token = jtNumber then
        R.AsDouble;
    WriteLn(Name, ': ok');
  except
    on E: EJsonError do
      WriteLn(Name, ': ', E.Message, ' [', E.Offset, ']');
  end;
  R.Free;
end;

procedure Number(const S: string);
var
  R: TJsonReader;
begin
  R := TJsonReader.Create(S);
  R.Read;
  case R.NumberKind of
    jnInt64: WriteLn(S, ' -> Int64 ', R.AsInt64);
    jnUInt64: WriteLn(S, ' -> UInt64 ', R.AsUInt64);
    jnDouble: WriteLn(S, ' -> Double ', R.AsDouble:30);
  end;
  R.Free;
end;

procedure Escaped(const S: string);
var
  R: TJsonReader;
  T: string;
  I: Integer;
begin
  R := TJsonReader.Create(S);
  R.Read;
  T := R.AsString;
  Write(S, ' ->');
  for I := 1 to Length(T) do
    Write(' ', IntToHex(Ord(T[I]), 2));
  WriteLn;
  R.Free;
end;

function c_strtod(p: PChar; endp: Pointer): Double; cdecl; external 'c' name 'strtod';

{ numbers with few digits and small exponents take the fast path: the same
  doubles as strtod }
procedure FastPath;
var
  Seed: QWord;
  I, J, Bad, Digits: Integer;
  S: string;
  R: TJsonReader;

  function Next(N: Integer): Integer;
  begin
    {$Q-}{$R-}
    Seed := Seed * 6364136223846793005 + 1442695040888963407;
    Result := Integer((Seed shr 33) mod QWord(N));
    {$Q+}{$R+}
  end;

begin
  Seed := 42;
  Bad := 0;
  for I := 1 to 20000 do
  begin
    S := '';
    if Next(2) = 0 then
      S := '-';
    Digits := 1 + Next(17);
    S := S + Chr(Ord('1') + Next(9));
    for J := 2 to Digits do
    begin
      if Next(6) = 0 then
        S := S + '.';
      S := S + Chr(Ord('0') + Next(10));
    end;
    if (Pos('.', S) > 0) and (S[Length(S)] = '.') then
      S := S + '5';
    if Next(3) = 0 then
      S := S + 'e' + IntToStr(Next(50) - 25);
    R := TJsonReader.Create(S);
    R.Read;
    if R.AsDouble <> c_strtod(PChar(S), nil) then
    begin
      Inc(Bad);
      if Bad < 5 then
        WriteLn('differs: ', S);
    end;
    R.Free;
  end;
  WriteLn('fast path: ', Bad, ' of 20000 differ');
end;

procedure SkipDemo;
var
  R: TJsonReader;
begin
  R := TJsonReader.Create('{"skip": {"a": [1, "x]\"y", {"b": null}]}, "keep": [true]}');
  R.Read;
  R.Read;
  Write(R.AsString, ' ');
  R.Skip;
  R.Read;
  Write(R.AsString, ' ');
  R.Read;
  Write(TokenNames[R.Token], ' ');
  R.Skip;
  Write(TokenNames[R.Token], ' ');
  R.Read;
  WriteLn(TokenNames[R.Token], ' ', R.Read);
  R.Free;
end;

var
  Deep: string;
  I: Integer;
begin
  Tokens('{"a": [1, 2.5, -3e2, true, false, null], "b": {"c": "d"}, "e": []}');
  Tokens('  "just a string"  ');
  Tokens('[[[]], {}]');
  Tokens('{"long key with more than eight bytes": "and a long value too, 0123456789"}');
  Tokens(#10'  {'#10'    "x": 1'#10'  }'#10);

  Check('y_object_empty_key', '{"":0}');
  Check('y_string_unicode', '"é𝄞 caf' + #$C3#$A9 + '"');
  Check('y_number_minus_zero', '-0');
  Check('y_structure_lonely_null', 'null');
  Check('y_string_escapes', '"\"\\\/\b\f\n\r\t"');
  Check('n_trailing_comma_array', '[1,]');
  Check('n_trailing_comma_object', '{"a":1,}');
  Check('n_missing_colon', '{"a" 1}');
  Check('n_key_not_string', '{1:2}');
  Check('n_unclosed_array', '[1, 2');
  Check('n_unclosed_string', '"abc');
  Check('n_bad_escape', '"\x"');
  Check('n_short_unicode', '"\u12"');
  Check('n_control_in_string', '"a' + #9 + 'b"');
  Check('n_leading_zero', '012');
  Check('n_plus', '+1');
  Check('n_dot_no_digits', '1.');
  Check('n_exp_no_digits', '1e+');
  Check('n_two_values', '1 2');
  Check('n_true_misspelled', 'tru');
  Check('n_comment', '[1] // no');
  Check('n_empty', '');
  Check('n_wrong_close', '[1}');
  Check('n_comma_first', '[,1]');
  Check('n_invalid_utf8', '"' + #$C3#$28 + '"');
  Check('n_overlong_utf8', '"' + #$C0#$AF + '"');
  Check('n_surrogate_utf8', '"' + #$ED#$A0#$80 + '"');
  Check('n_lone_surrogate', '"\uD800"');
  Check('n_lone_low_surrogate', '"\uDC00x"');
  Check('n_position', '{'#10'  "a": [1, 2'#10'   3]}');
  Deep := '';
  for I := 1 to 600 do
    Deep := Deep + '[';
  Check('n_too_deep', Deep);

  Number('0');
  Number('-17');
  Number('9223372036854775807');
  Number('-9223372036854775808');
  Number('9223372036854775808');
  Number('18446744073709551615');
  Number('18446744073709551616');
  Number('0.1');
  Number('1.5e3');
  Number('-2.5E-3');
  Number('123456789012345678901234567890');
  Check('n_number_too_large', '1e400');
  Number('4.9406564584124654e-324');
  Number('0.30000000000000004');

  Escaped('"Aé€😀"');
  Escaped('"tab\there"');

  WriteLn(JsonQuote('say "hi"\' + #10 + #1 + ' and ' + #$C3#$A9));
  WriteLn(JsonValidUtf8('caf' + #$C3#$A9), ' ', JsonValidUtf8(#$FF));
  SkipDemo;
  FastPath;
end.
