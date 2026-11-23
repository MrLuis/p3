unit json;
{ JSON, after Rust's sonic-rs (github.com/cloudwego/sonic-rs).

  TJsonReader is a pull reader: Read moves to the next token (the start or
  end of an object or array, a key, a string, a number, true, false, null)
  and checks the grammar on the way; the token's value is read with
  AsString, AsInt64, AsDouble, ... Skip passes over a whole value. Everything
  else in this unit is built on it.

  Speed: text is scanned 8 bytes at a time where it can be (whitespace runs,
  strings: looking for '"', '\', control characters and non-ASCII bytes in a
  qword at once); a string without escapes is copied once, when it is read;
  numbers are converted when they are read, exactly: integers to Int64 or
  UInt64, others to the nearest double (directly for up to 15 digits and
  powers of ten up to 22, else through the C library's strtod).

  Errors raise EJsonError, with the offset, line and column of the problem
  and a message such as  expected ',' or ']' at line 1 column 17.
  Strings are checked to be valid UTF-8 unless the reader is created with
  jroNoUtf8Check. }

{$H+}
{$modeswitch advancedrecords}

interface

uses sysutils;

type
  EJsonError = class(Exception)
  public
    { where the problem is: a byte offset (0-based), a line and a column
      (1-based, the column in bytes) }
    Offset, Line, Column: Integer;
    { the message without the position }
    Reason: string;
  end;

  TJsonToken = (jtNone, jtBeginObject, jtEndObject, jtBeginArray, jtEndArray,
                jtKey, jtString, jtNumber, jtTrue, jtFalse, jtNull, jtEnd);

  { what a number is, once read: an integer that fits Int64, one that fits
    only UInt64, or a double (a fraction, an exponent, or too large) }
  TJsonNumberKind = (jnInt64, jnUInt64, jnDouble);

  TJsonReaderOption = (jroNoUtf8Check);
  TJsonReaderOptions = set of TJsonReaderOption;

  TJsonReader = class
  private
    FText: string;
    FP: PChar;
    FLen, FPos: Integer;
    FToken: TJsonToken;
    FStart, FStop: Integer;   { the token's text: a string's inside quotes }
    FEscaped: Boolean;        { a string token with escapes }
    FNumParsed: Boolean;
    FNumKind: TJsonNumberKind;
    FInt: Int64;
    FUInt: QWord;
    FDouble: Double;
    FStack: array of Boolean; { per open container: is it an object }
    FDepth, FMaxDepth: Integer;
    FState: Integer;
    FOptions: TJsonReaderOptions;
    procedure Fail(const Reason: string; At: Integer);
    procedure SkipWhitespace; inline;
    procedure ReadValue;
    procedure ReadStringToken;
    procedure ReadNumberToken;
    procedure ReadLiteral(const Word: string; T: TJsonToken);
    procedure Push(IsObject: Boolean);
    procedure ParseNumber;
    procedure SkipContainer;
    function GetDepth: Integer;
  public
    constructor Create(const Text: string; Options: TJsonReaderOptions = []);
    { Moves to the next token; false at the end of the text (Token jtEnd),
      after one whole value. }
    function Read: Boolean;
    { Passes over the current value: after a jtBeginObject or jtBeginArray
      token, to its end (checking brackets and strings only, which is fast);
      after a key, over the key's value; else nothing. }
    procedure Skip;
    property Token: TJsonToken read FToken;
    { the current string or key, decoded }
    function AsString: string;
    { the current token's text as written (a string's without quotes) }
    function RawText: string;
    function IsInteger: Boolean;
    function NumberKind: TJsonNumberKind;
    function AsInt64: Int64;
    function AsUInt64: QWord;
    function AsDouble: Double;
    function AsBoolean: Boolean;
    { the current string or key equals S (no decoding when it has no
      escapes) }
    function StringEquals(const S: string): Boolean;
    { the offset of the current token (its opening quote, for a string) }
    property TokenStart: Integer read FStart;
    property Position: Integer read FPos;
    property Depth: Integer read GetDepth;
    { the most nested containers allowed (512 by default) }
    property MaxDepth: Integer read FMaxDepth write FMaxDepth;
    property Text: string read FText;
    { raises EJsonError at offset At }
    procedure Error(const Reason: string; At: Integer);
  end;

{ The line and column (1-based) of byte offset Offset of Text. }
procedure JsonLineColumn(const Text: string; Offset: Integer;
                         out Line, Column: Integer);
{ S as a JSON string literal, with its quotes. }
function JsonQuote(const S: string): string;
{ appends S, escaped, without quotes }
procedure JsonEscapeTo(var Buf: string; var Len: Integer; const S: string);
{ Is S valid UTF-8? }
function JsonValidUtf8(const S: string): Boolean;

implementation

{$R-}{$Q-}

type
  PQWord = ^QWord;

const
  Ones = QWord($0101010101010101);
  Highs = QWord($8080808080808080);
  Spaces8 = QWord($2020202020202020);

  { reader states }
  rsStart = 0;        { the top-level value comes }
  rsDone = 1;         { after it: only whitespace }
  rsFirstInObject = 2;
  rsFirstInArray = 3;
  rsAfterValue = 4;   { in a container: ',' or its end }
  rsAfterKey = 5;     { ':' and a value }
  rsKeyAfterComma = 6;
  rsValueAfterComma = 7;

  Pow10: array[0..22] of Double = (1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7,
    1e8, 1e9, 1e10, 1e11, 1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19,
    1e20, 1e21, 1e22);

function c_strtod(p: PChar; endp: Pointer): Double; cdecl; external 'c' name 'strtod';

{ Does W contain a byte B? }
function HasByte(W: QWord; B: Byte): Boolean; inline;
var
  X: QWord;
begin
  X := W xor (Ones * B);
  Result := ((X - Ones) and not X and Highs) <> 0;
end;

{ Does W contain a byte below 32? }
function HasControl(W: QWord): Boolean; inline;
begin
  Result := ((W - Ones * 32) and not W and Highs) <> 0;
end;

procedure JsonLineColumn(const Text: string; Offset: Integer;
                         out Line, Column: Integer);
var
  I: Integer;
begin
  Line := 1;
  Column := 1;
  if Offset > Length(Text) then
    Offset := Length(Text);
  for I := 1 to Offset do
    if Text[I] = #10 then
    begin
      Inc(Line);
      Column := 1;
    end
    else
      Inc(Column);
end;

{ The length of the UTF-8 sequence at P[I] (of Len bytes), 0 if invalid. }
function Utf8SequenceLength(P: PChar; I, Len: Integer): Integer;
var
  B, B1: Byte;
  N, K: Integer;
begin
  Result := 0;
  B := Ord(P[I]);
  if B < $80 then
    Exit(1);
  if (B >= $C2) and (B <= $DF) then
    N := 2
  else if (B >= $E0) and (B <= $EF) then
    N := 3
  else if (B >= $F0) and (B <= $F4) then
    N := 4
  else
    Exit;
  if I + N > Len then
    Exit;
  B1 := Ord(P[I + 1]);
  case B of
    $E0: if (B1 < $A0) or (B1 > $BF) then Exit;
    $ED: if (B1 < $80) or (B1 > $9F) then Exit;
    $F0: if (B1 < $90) or (B1 > $BF) then Exit;
    $F4: if (B1 < $80) or (B1 > $8F) then Exit;
  else
    if (B1 < $80) or (B1 > $BF) then Exit;
  end;
  for K := 2 to N - 1 do
    if (Ord(P[I + K]) < $80) or (Ord(P[I + K]) > $BF) then
      Exit;
  Result := N;
end;

function JsonValidUtf8(const S: string): Boolean;
var
  P: PChar;
  I, N, Len: Integer;
begin
  P := PChar(S);
  Len := Length(S);
  I := 0;
  while I < Len do
  begin
    if (I + 8 <= Len) and (PQWord(P + I)^ and Highs = 0) then
    begin
      Inc(I, 8);
      Continue;
    end;
    N := Utf8SequenceLength(P, I, Len);
    if N = 0 then
      Exit(False);
    Inc(I, N);
  end;
  Result := True;
end;

{ --- escaping --- }

const
  HexDigits: array[0..15] of Char = ('0', '1', '2', '3', '4', '5', '6', '7',
    '8', '9', 'a', 'b', 'c', 'd', 'e', 'f');

procedure AddChars(var Buf: string; var Len: Integer; P: PChar; N: Integer);
var
  Cap: Integer;
begin
  if N <= 0 then
    Exit;
  Cap := Length(Buf);
  if Len + N > Cap then
  begin
    Cap := Cap * 2;
    if Cap < Len + N then
      Cap := Len + N + 64;
    SetLength(Buf, Cap);
  end;
  Move(P^, Buf[Len + 1], N);
  Inc(Len, N);
end;

procedure AddChar(var Buf: string; var Len: Integer; C: Char); inline;
begin
  if Len + 1 > Length(Buf) then
    SetLength(Buf, Length(Buf) * 2 + 64);
  Inc(Len);
  Buf[Len] := C;
end;

procedure JsonEscapeTo(var Buf: string; var Len: Integer; const S: string);
var
  P: PChar;
  I, Run, N: Integer;
  W: QWord;
  C: Char;
begin
  P := PChar(S);
  N := Length(S);
  I := 0;
  Run := 0; { the start of the bytes not yet copied }
  while I < N do
  begin
    if I + 8 <= N then
    begin
      W := PQWord(P + I)^;
      if not HasByte(W, Ord('"')) and not HasByte(W, Ord('\')) and
         not HasControl(W) then
      begin
        Inc(I, 8);
        Continue;
      end;
    end;
    C := P[I];
    if (C = '"') or (C = '\') or (C < ' ') then
    begin
      AddChars(Buf, Len, P + Run, I - Run);
      AddChar(Buf, Len, '\');
      case C of
        '"': AddChar(Buf, Len, '"');
        '\': AddChar(Buf, Len, '\');
        #8: AddChar(Buf, Len, 'b');
        #9: AddChar(Buf, Len, 't');
        #10: AddChar(Buf, Len, 'n');
        #12: AddChar(Buf, Len, 'f');
        #13: AddChar(Buf, Len, 'r');
      else
        begin
          AddChar(Buf, Len, 'u');
          AddChar(Buf, Len, '0');
          AddChar(Buf, Len, '0');
          AddChar(Buf, Len, HexDigits[Ord(C) shr 4]);
          AddChar(Buf, Len, HexDigits[Ord(C) and 15]);
        end;
      end;
      Run := I + 1;
    end;
    Inc(I);
  end;
  AddChars(Buf, Len, P + Run, N - Run);
end;

function JsonQuote(const S: string): string;
var
  Buf: string;
  Len: Integer;
begin
  SetLength(Buf, Length(S) + 16);
  Len := 0;
  AddChar(Buf, Len, '"');
  JsonEscapeTo(Buf, Len, S);
  AddChar(Buf, Len, '"');
  SetLength(Buf, Len);
  Result := Buf;
end;

{ --- the reader --- }

constructor TJsonReader.Create(const Text: string; Options: TJsonReaderOptions);
begin
  FText := Text;
  FP := PChar(FText);
  FLen := Length(FText);
  FPos := 0;
  FToken := jtNone;
  FState := rsStart;
  FDepth := 0;
  FMaxDepth := 512;
  FOptions := Options;
end;

procedure TJsonReader.Fail(const Reason: string; At: Integer);
var
  E: EJsonError;
  L, C: Integer;
begin
  JsonLineColumn(FText, At, L, C);
  E := EJsonError.Create(Reason + ' at line ' + IntToStr(L) + ' column ' +
                         IntToStr(C));
  E.Reason := Reason;
  E.Offset := At;
  E.Line := L;
  E.Column := C;
  raise E;
end;

procedure TJsonReader.Error(const Reason: string; At: Integer);
begin
  Fail(Reason, At);
end;

function TJsonReader.GetDepth: Integer;
begin
  Result := FDepth;
end;

procedure TJsonReader.SkipWhitespace;
var
  C: Char;
begin
  while FPos < FLen do
  begin
    C := FP[FPos];
    if C = ' ' then
    begin
      { (indentation: runs of spaces, 8 at a time) }
      while (FPos + 8 <= FLen) and (PQWord(FP + FPos)^ = Spaces8) do
        Inc(FPos, 8);
      Inc(FPos);
    end
    else if (C = #10) or (C = #13) or (C = #9) then
      Inc(FPos)
    else
      Break;
  end;
end;

procedure TJsonReader.Push(IsObject: Boolean);
begin
  if FDepth >= FMaxDepth then
    Fail('recursion limit exceeded (more than ' + IntToStr(FMaxDepth) +
         ' nested arrays and objects)', FPos);
  if FDepth >= Length(FStack) then
    SetLength(FStack, Length(FStack) * 2 + 16);
  FStack[FDepth] := IsObject;
  Inc(FDepth);
end;

procedure TJsonReader.ReadLiteral(const Word: string; T: TJsonToken);
var
  I: Integer;
begin
  FStart := FPos;
  for I := 1 to Length(Word) do
  begin
    if FPos >= FLen then
      Fail('EOF while parsing a value', FPos);
    if FP[FPos] <> Word[I] then
      Fail('expected value', FStart);
    Inc(FPos);
  end;
  FStop := FPos;
  FToken := T;
end;

procedure TJsonReader.ReadStringToken;
var
  I, N: Integer;
  W: QWord;
  C: Char;
  Check: Boolean;
begin
  FStart := FPos; { the quote }
  FEscaped := False;
  Check := not (jroNoUtf8Check in FOptions);
  I := FPos + 1;
  while True do
  begin
    { 8 bytes at a time while none ends the string or needs a look }
    while I + 8 <= FLen do
    begin
      W := PQWord(FP + I)^;
      if HasByte(W, Ord('"')) or HasByte(W, Ord('\')) or HasControl(W) or
         (Check and (W and Highs <> 0)) then
        Break;
      Inc(I, 8);
    end;
    if I >= FLen then
      Fail('EOF while parsing a string', FLen);
    C := FP[I];
    if C = '"' then
      Break;
    if C = '\' then
    begin
      FEscaped := True;
      if I + 1 >= FLen then
        Fail('EOF while parsing a string', FLen);
      case FP[I + 1] of
        '"', '\', '/', 'b', 'f', 'n', 'r', 't': Inc(I, 2);
        'u':
          begin
            for N := 2 to 5 do
            begin
              if I + N >= FLen then
                Fail('EOF while parsing a string', FLen);
              if not (FP[I + N] in ['0'..'9', 'a'..'f', 'A'..'F']) then
                Fail('invalid escape', I + N);
            end;
            Inc(I, 6);
          end;
      else
        Fail('invalid escape', I + 1);
      end;
    end
    else if C < ' ' then
      Fail('control character (\u' + HexDigits[Ord(C) shr 4] +
           HexDigits[Ord(C) and 15] + ') while parsing a string', I)
    else if (C >= #128) and Check then
    begin
      N := Utf8SequenceLength(FP, I, FLen);
      if N = 0 then
        Fail('invalid UTF-8 in a string', I);
      Inc(I, N);
    end
    else
      Inc(I);
  end;
  FStart := FStart + 1;
  FStop := I;
  FPos := I + 1;
end;

procedure TJsonReader.ReadNumberToken;
var
  I: Integer;
begin
  FStart := FPos;
  I := FPos;
  if FP[I] = '-' then
    Inc(I);
  if I >= FLen then
    Fail('EOF while parsing a value', I);
  if FP[I] = '0' then
  begin
    Inc(I);
    if (I < FLen) and (FP[I] in ['0'..'9']) then
      Fail('invalid number', I);
  end
  else if FP[I] in ['1'..'9'] then
    while (I < FLen) and (FP[I] in ['0'..'9']) do
      Inc(I)
  else
    Fail('invalid number', I);
  if (I < FLen) and (FP[I] = '.') then
  begin
    Inc(I);
    if (I >= FLen) or not (FP[I] in ['0'..'9']) then
      Fail('invalid number', I);
    while (I < FLen) and (FP[I] in ['0'..'9']) do
      Inc(I);
  end;
  if (I < FLen) and (FP[I] in ['e', 'E']) then
  begin
    Inc(I);
    if (I < FLen) and (FP[I] in ['+', '-']) then
      Inc(I);
    if (I >= FLen) or not (FP[I] in ['0'..'9']) then
      Fail('invalid number', I);
    while (I < FLen) and (FP[I] in ['0'..'9']) do
      Inc(I);
  end;
  FStop := I;
  FPos := I;
  FToken := jtNumber;
  FNumParsed := False;
end;

procedure TJsonReader.ReadValue;
begin
  if FPos >= FLen then
    Fail('EOF while parsing a value', FPos);
  case FP[FPos] of
    '{':
      begin
        FStart := FPos;
        Push(True);
        Inc(FPos);
        FToken := jtBeginObject;
        FState := rsFirstInObject;
        Exit;
      end;
    '[':
      begin
        FStart := FPos;
        Push(False);
        Inc(FPos);
        FToken := jtBeginArray;
        FState := rsFirstInArray;
        Exit;
      end;
    '"':
      begin
        ReadStringToken;
        FToken := jtString;
      end;
    '-', '0'..'9': ReadNumberToken;
    't': ReadLiteral('true', jtTrue);
    'f': ReadLiteral('false', jtFalse);
    'n': ReadLiteral('null', jtNull);
  else
    Fail('expected value', FPos);
  end;
  if FDepth = 0 then
    FState := rsDone
  else
    FState := rsAfterValue;
end;

function TJsonReader.Read: Boolean;
var
  C: Char;
begin
  SkipWhitespace;
  case FState of
    rsStart:
      ReadValue;
    rsDone:
      begin
        if FPos < FLen then
          Fail('trailing characters', FPos);
        FToken := jtEnd;
        Exit(False);
      end;
    rsFirstInObject, rsKeyAfterComma:
      begin
        if FPos >= FLen then
          Fail('EOF while parsing an object', FPos);
        C := FP[FPos];
        if (C = '}') and (FState = rsFirstInObject) then
        begin
          FStart := FPos;
          Inc(FPos);
          Dec(FDepth);
          FToken := jtEndObject;
          if FDepth = 0 then FState := rsDone else FState := rsAfterValue;
        end
        else if C = '"' then
        begin
          ReadStringToken;
          FToken := jtKey;
          FState := rsAfterKey;
        end
        else if C = '}' then
          Fail('trailing comma', FPos)
        else
          Fail('key must be a string', FPos);
      end;
    rsFirstInArray:
      begin
        if FPos >= FLen then
          Fail('EOF while parsing a list', FPos);
        if FP[FPos] = ']' then
        begin
          FStart := FPos;
          Inc(FPos);
          Dec(FDepth);
          FToken := jtEndArray;
          if FDepth = 0 then FState := rsDone else FState := rsAfterValue;
        end
        else
          ReadValue;
      end;
    rsValueAfterComma:
      begin
        if FPos >= FLen then
          Fail('EOF while parsing a list', FPos);
        if FP[FPos] = ']' then
          Fail('trailing comma', FPos);
        ReadValue;
      end;
    rsAfterKey:
      begin
        if (FPos >= FLen) or (FP[FPos] <> ':') then
        begin
          if FPos >= FLen then
            Fail('EOF while parsing an object', FPos);
          Fail('expected '':''', FPos);
        end;
        Inc(FPos);
        SkipWhitespace;
        ReadValue;
      end;
    rsAfterValue:
      begin
        if FStack[FDepth - 1] then
        begin
          if FPos >= FLen then
            Fail('EOF while parsing an object', FPos);
          C := FP[FPos];
          if C = ',' then
          begin
            Inc(FPos);
            FState := rsKeyAfterComma;
            Exit(Read);
          end;
          if C <> '}' then
            Fail('expected '','' or ''}''', FPos);
          FToken := jtEndObject;
        end
        else
        begin
          if FPos >= FLen then
            Fail('EOF while parsing a list', FPos);
          C := FP[FPos];
          if C = ',' then
          begin
            Inc(FPos);
            FState := rsValueAfterComma;
            Exit(Read);
          end;
          if C <> ']' then
            Fail('expected '','' or '']''', FPos);
          FToken := jtEndArray;
        end;
        FStart := FPos;
        Inc(FPos);
        Dec(FDepth);
        if FDepth = 0 then FState := rsDone else FState := rsAfterValue;
      end;
  end;
  Result := True;
end;

{ Over the rest of the container just opened: brackets and strings only. }
procedure TJsonReader.SkipContainer;
var
  Level, I: Integer;
  W: QWord;
  C: Char;
begin
  Level := 1;
  I := FPos;
  while I < FLen do
  begin
    C := FP[I];
    case C of
      '"':
        begin
          Inc(I);
          while True do
          begin
            while (I + 8 <= FLen) do
            begin
              W := PQWord(FP + I)^;
              if HasByte(W, Ord('"')) or HasByte(W, Ord('\')) then
                Break;
              Inc(I, 8);
            end;
            if I >= FLen then
              Fail('EOF while parsing a string', FLen);
            if FP[I] = '"' then
              Break;
            if FP[I] = '\' then
              Inc(I, 2)
            else
              Inc(I);
          end;
        end;
      '{', '[': Inc(Level);
      '}', ']':
        begin
          Dec(Level);
          if Level = 0 then
          begin
            FStart := I;
            FPos := I + 1;
            if C = '}' then FToken := jtEndObject else FToken := jtEndArray;
            Dec(FDepth);
            if FDepth = 0 then FState := rsDone else FState := rsAfterValue;
            Exit;
          end;
        end;
    end;
    Inc(I);
  end;
  Fail('EOF while parsing a value', FLen);
end;

procedure TJsonReader.Skip;
begin
  case FToken of
    jtBeginObject, jtBeginArray: SkipContainer;
    jtKey:
      begin
        Read;
        if FToken in [jtBeginObject, jtBeginArray] then
          SkipContainer;
      end;
  end;
end;

function TJsonReader.RawText: string;
begin
  Result := Copy(FText, FStart + 1, FStop - FStart);
end;

function HexValue(C: Char): Integer; inline;
begin
  case C of
    '0'..'9': Result := Ord(C) - Ord('0');
    'a'..'f': Result := Ord(C) - Ord('a') + 10;
  else
    Result := Ord(C) - Ord('A') + 10;
  end;
end;

function TJsonReader.AsString: string;
var
  I, Len, Run, CP, Lo: Integer;
  C: Char;
begin
  if not (FToken in [jtString, jtKey]) then
    Fail('not a string', FStart);
  if not FEscaped then
    Exit(Copy(FText, FStart + 1, FStop - FStart));
  SetLength(Result, FStop - FStart);
  Len := 0;
  I := FStart;
  Run := I;
  while I < FStop do
  begin
    if FP[I] <> '\' then
    begin
      Inc(I);
      Continue;
    end;
    AddChars(Result, Len, FP + Run, I - Run);
    C := FP[I + 1];
    Inc(I, 2);
    case C of
      'b': AddChar(Result, Len, #8);
      'f': AddChar(Result, Len, #12);
      'n': AddChar(Result, Len, #10);
      'r': AddChar(Result, Len, #13);
      't': AddChar(Result, Len, #9);
      'u':
        begin
          CP := (HexValue(FP[I]) shl 12) or (HexValue(FP[I + 1]) shl 8) or
                (HexValue(FP[I + 2]) shl 4) or HexValue(FP[I + 3]);
          Inc(I, 4);
          if (CP >= $D800) and (CP <= $DBFF) then
          begin
            { a surrogate pair: the low half must follow }
            if (I + 6 > FStop) or (FP[I] <> '\') or (FP[I + 1] <> 'u') then
              Fail('lone leading surrogate in hex escape', I - 6);
            Lo := (HexValue(FP[I + 2]) shl 12) or (HexValue(FP[I + 3]) shl 8) or
                  (HexValue(FP[I + 4]) shl 4) or HexValue(FP[I + 5]);
            if (Lo < $DC00) or (Lo > $DFFF) then
              Fail('invalid surrogate pair in hex escape', I);
            Inc(I, 6);
            CP := $10000 + ((CP - $D800) shl 10) + (Lo - $DC00);
          end
          else if (CP >= $DC00) and (CP <= $DFFF) then
            Fail('lone trailing surrogate in hex escape', I - 6);
          if CP < $80 then
            AddChar(Result, Len, Chr(CP))
          else if CP < $800 then
          begin
            AddChar(Result, Len, Chr($C0 or (CP shr 6)));
            AddChar(Result, Len, Chr($80 or (CP and $3F)));
          end
          else if CP < $10000 then
          begin
            AddChar(Result, Len, Chr($E0 or (CP shr 12)));
            AddChar(Result, Len, Chr($80 or ((CP shr 6) and $3F)));
            AddChar(Result, Len, Chr($80 or (CP and $3F)));
          end
          else
          begin
            AddChar(Result, Len, Chr($F0 or (CP shr 18)));
            AddChar(Result, Len, Chr($80 or ((CP shr 12) and $3F)));
            AddChar(Result, Len, Chr($80 or ((CP shr 6) and $3F)));
            AddChar(Result, Len, Chr($80 or (CP and $3F)));
          end;
        end;
    else
      AddChar(Result, Len, C); { " \ / }
    end;
    Run := I;
  end;
  AddChars(Result, Len, FP + Run, FStop - Run);
  SetLength(Result, Len);
end;

function TJsonReader.StringEquals(const S: string): Boolean;
var
  N, I: Integer;
  Q: PChar;
begin
  if FEscaped then
    Exit(AsString = S);
  N := FStop - FStart;
  if N <> Length(S) then
    Exit(False);
  Q := PChar(S);
  I := 0;
  while I + 8 <= N do
  begin
    if PQWord(FP + FStart + I)^ <> PQWord(Q + I)^ then
      Exit(False);
    Inc(I, 8);
  end;
  while I < N do
  begin
    if FP[FStart + I] <> Q[I] then
      Exit(False);
    Inc(I);
  end;
  Result := True;
end;

procedure TJsonReader.ParseNumber;
var
  I, Digits, Frac, Exp10, ExpVal: Integer;
  Neg, ExpNeg, Big: Boolean;
  M: QWord;
  D: Double;
  Buf: string;
begin
  if FToken <> jtNumber then
    Fail('not a number', FStart);
  if FNumParsed then
    Exit;
  FNumParsed := True;
  I := FStart;
  Neg := FP[I] = '-';
  if Neg then
    Inc(I);
  M := 0;
  Digits := 0;
  Big := False;
  while (I < FStop) and (FP[I] in ['0'..'9']) do
  begin
    if Digits < 19 then
      M := M * 10 + QWord(Ord(FP[I]) - Ord('0'))
    else if Digits = 19 then
    begin
      { the 20th digit: still a UInt64 if no overflow }
      if (M > QWord(1844674407370955161)) or
         ((M = QWord(1844674407370955161)) and (FP[I] > '5')) then
        Big := True
      else
        M := M * 10 + QWord(Ord(FP[I]) - Ord('0'));
    end
    else
      Big := True;
    Inc(Digits);
    Inc(I);
  end;
  if (I = FStop) and not Big then
  begin
    { an integer }
    if Neg then
    begin
      if M <= QWord(High(Int64)) + 1 then
      begin
        FNumKind := jnInt64;
        FInt := -Int64(M - 1) - 1;
        FDouble := FInt;
        Exit;
      end;
    end
    else if M <= QWord(High(Int64)) then
    begin
      FNumKind := jnInt64;
      FInt := Int64(M);
      FDouble := M;
      Exit;
    end
    else
    begin
      FNumKind := jnUInt64;
      FUInt := M;
      FDouble := M;
      Exit;
    end;
  end;
  FNumKind := jnDouble;
  { the fast path: few digits, a small power of ten (exact in a double) }
  if not Big then
  begin
    Frac := 0;
    if (I < FStop) and (FP[I] = '.') then
    begin
      Inc(I);
      while (I < FStop) and (FP[I] in ['0'..'9']) do
      begin
        if Digits < 19 then
          M := M * 10 + QWord(Ord(FP[I]) - Ord('0'))
        else
          Big := True;
        Inc(Digits);
        Inc(Frac);
        Inc(I);
      end;
    end;
    Exp10 := 0;
    if (I < FStop) and (FP[I] in ['e', 'E']) then
    begin
      Inc(I);
      ExpNeg := FP[I] = '-';
      if FP[I] in ['+', '-'] then
        Inc(I);
      ExpVal := 0;
      while I < FStop do
      begin
        if ExpVal < 100000 then
          ExpVal := ExpVal * 10 + (Ord(FP[I]) - Ord('0'));
        Inc(I);
      end;
      if ExpNeg then Exp10 := -ExpVal else Exp10 := ExpVal;
    end;
    Exp10 := Exp10 - Frac;
    if not Big and (M <= QWord(1) shl 53) and (Exp10 >= -22) and
       (Exp10 <= 22) then
    begin
      D := M;
      if Exp10 >= 0 then
        FDouble := D * Pow10[Exp10]
      else
        FDouble := D / Pow10[-Exp10];
      if Neg then
        FDouble := -FDouble;
      Exit;
    end;
  end;
  Buf := Copy(FText, FStart + 1, FStop - FStart);
  FDouble := c_strtod(PChar(Buf), nil);
  if FDouble - FDouble <> 0 then { (infinite) }
  begin
    FNumParsed := False;
    Fail('number out of range', FStart);
  end;
end;

function TJsonReader.IsInteger: Boolean;
begin
  ParseNumber;
  Result := FNumKind <> jnDouble;
end;

function TJsonReader.NumberKind: TJsonNumberKind;
begin
  ParseNumber;
  Result := FNumKind;
end;

function TJsonReader.AsInt64: Int64;
begin
  ParseNumber;
  if FNumKind <> jnInt64 then
    Fail('number out of range of Int64 or not an integer', FStart);
  Result := FInt;
end;

function TJsonReader.AsUInt64: QWord;
begin
  ParseNumber;
  if (FNumKind = jnInt64) and (FInt >= 0) then
    Result := QWord(FInt)
  else if FNumKind = jnUInt64 then
    Result := FUInt
  else
    Fail('number out of range of UInt64 or not an integer', FStart);
end;

function TJsonReader.AsDouble: Double;
begin
  ParseNumber;
  Result := FDouble;
end;

function TJsonReader.AsBoolean: Boolean;
begin
  case FToken of
    jtTrue: Result := True;
    jtFalse: Result := False;
  else
    Fail('not a boolean', FStart);
  end;
end;

end.
