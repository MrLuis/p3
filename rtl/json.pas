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
    { the end of the current token's text (a string's closing quote) }
    property TokenStop: Integer read FStop;
    { the current string or key has escapes }
    property TokenEscaped: Boolean read FEscaped;
    property Position: Integer read FPos;
    property Depth: Integer read GetDepth;
    { the most nested containers allowed (512 by default) }
    property MaxDepth: Integer read FMaxDepth write FMaxDepth;
    property Text: string read FText;
    { raises EJsonError at offset At }
    procedure Error(const Reason: string; At: Integer);
  end;

  { --- documents ---

    TJsonDocument holds a parsed (or built) JSON value: its nodes are in
    the document's own memory, in large blocks; the strings and numbers
    read from the text stay there until used (a string is decoded when
    read, and written back as it was). A TJsonValue is a handle on a node
    of a document, valid while the document is: copy it freely. }

  TJsonKind = (jkNull, jkFalse, jkTrue, jkNumber, jkString, jkArray, jkObject);

  TJsonDocument = class;
  TJsonWriter = class;

  PJsonNode = ^TJsonNode;
  TJsonNode = record
    Kind: TJsonKind;
    NumKind: TJsonNumberKind;
    InSource: Boolean; { a string or number in the source text }
    Escaped: Boolean;  { (a source string with escapes) }
    Start, Len: Integer;
    Int: Int64;        { an integer (UInt64: its bits) }
    Dbl: Double;
    Str: string;       { a string not in the source (or decoded) }
    Count: Integer;    { an array's or object's members }
    Items: array of PJsonNode;
    Keys: array of PJsonNode; { an object's: string nodes }
  end;

  { the index of V[...]: a key or an array index }
  TJsonKey = record
    Name: string;
    Index: Integer; { -1 for a name }
    class operator :=(const S: string): TJsonKey;
    class operator :=(I: Integer): TJsonKey;
  end;

  TJsonValue = record
  private
    FNode: PJsonNode;
    FDoc: TJsonDocument;
    function GetItem(const K: TJsonKey): TJsonValue;
    procedure Need(K: TJsonKind; const What: string);
    function NewChild(K: TJsonKind): PJsonNode;
    procedure AddNode(N: PJsonNode);
    procedure PutNode(const Key: string; N: PJsonNode);
    function Adopt(const V: TJsonValue): PJsonNode;
  public
    { a value that is not there: V['missing'], V[99] }
    function Exists: Boolean;
    function Kind: TJsonKind;
    function IsNull: Boolean;
    function IsBoolean: Boolean;
    function IsNumber: Boolean;
    function IsInteger: Boolean;
    function IsString: Boolean;
    function IsArray: Boolean;
    function IsObject: Boolean;
    { these raise EJsonError when the value is of another kind (or is not
      there); AsInt64 also for a number that is not an Int64 }
    function AsString: string;
    function AsInt64: Int64;
    function AsUInt64: QWord;
    function AsDouble: Double;
    function AsBoolean: Boolean;
    { ... and these give Default instead }
    function AsStringDef(const Default: string): string;
    function AsInt64Def(Default: Int64): Int64;
    function AsDoubleDef(Default: Double): Double;
    function AsBooleanDef(Default: Boolean): Boolean;
    { the members of an array or object (0 for the others) }
    function Count: Integer;
    { an object's member (first of the key; not there: not Exists) }
    function Get(const Key: string): TJsonValue;
    function TryGet(const Key: string; out V: TJsonValue): Boolean;
    function Has(const Key: string): Boolean;
    { an array's element, or an object's I-th value }
    function At(I: Integer): TJsonValue;
    { an object's I-th key }
    function KeyAt(I: Integer): string;
    { V['name'], V[0], V['a'][0]['b'] }
    property Items[const K: TJsonKey]: TJsonValue read GetItem; default;
    { by a JSON Pointer (RFC 6901): '/a/0/b' ('' is V itself) }
    function Path(const Pointer: string): TJsonValue;
    { changes: an object's member (replaced if the key is there) ... }
    procedure Put(const Key: string; const S: string); overload;
    procedure Put(const Key: string; I: Int64); overload;
    procedure Put(const Key: string; D: Double); overload;
    procedure Put(const Key: string; B: Boolean); overload;
    procedure Put(const Key: string; const V: TJsonValue); overload;
    procedure PutNull(const Key: string);
    function PutObject(const Key: string): TJsonValue;
    function PutArray(const Key: string): TJsonValue;
    { ... an array's element, at its end }
    procedure Add(const S: string); overload;
    procedure Add(I: Int64); overload;
    procedure Add(D: Double); overload;
    procedure Add(B: Boolean); overload;
    procedure Add(const V: TJsonValue); overload;
    procedure AddNull;
    function AddObject: TJsonValue;
    function AddArray: TJsonValue;
    { removes an object's member (false if it is not there) }
    function Delete(const Key: string): Boolean;
    { removes an array's element }
    procedure Remove(I: Integer);
    procedure Clear;
    { the value as JSON text }
    function ToJson(Pretty: Boolean = False): string;
    procedure WriteTo(W: TJsonWriter);
    property Document: TJsonDocument read FDoc;
  end;

  TJsonMember = record
    Key: string;
    Value: TJsonValue;
  end;

  TJsonDocument = class
  private
    FSource: string;
    FBlocks: array of array of TJsonNode;
    FUsed: Integer;
    FRoot: PJsonNode;
    function NewNode(K: TJsonKind): PJsonNode;
    function Handle(N: PJsonNode): TJsonValue;
    function NodeString(N: PJsonNode): string;
    function CopyNode(Src: TJsonDocument; N: PJsonNode): PJsonNode;
  public
    { an empty document (its root null) }
    constructor Create;
    destructor Destroy; override;
    { parses Text into this document (replacing what it had) }
    procedure Load(const Text: string; Options: TJsonReaderOptions = []);
    function Root: TJsonValue;
    procedure SetRoot(const V: TJsonValue);
    { values of this document not yet placed (Put, Add or SetRoot them) }
    function NewObject: TJsonValue;
    function NewArray: TJsonValue;
    function NewString(const S: string): TJsonValue;
    function NewInteger(I: Int64): TJsonValue;
    function NewNumber(D: Double): TJsonValue;
    function NewBoolean(B: Boolean): TJsonValue;
    function NewNull: TJsonValue;
    function ToJson(Pretty: Boolean = False): string;
  end;

  { --- writing ---

    TJsonWriter writes JSON text: compact, or pretty (two spaces for each
    level, as serde_json's to_string_pretty). Commas and colons are its
    business; doubles are written as the shortest text that reads back as
    the same double. }
  TJsonWriter = class
  private
    FBuf: string;
    FLen: Integer;
    FPretty: Boolean;
    FCount: array of Integer; { per open container: its members so far }
    FDepth: Integer;
    FAfterKey: Boolean;
    procedure BeforeValue;
    procedure Open(C: Char);
    procedure Close(C: Char);
    procedure NewLine;
  public
    constructor Create(Pretty: Boolean = False);
    procedure BeginObject;
    procedure EndObject;
    procedure BeginArray;
    procedure EndArray;
    procedure Key(const K: string);
    procedure Value(const S: string); overload;
    procedure Value(I: Int64); overload;
    procedure Value(D: Double); overload;
    procedure Value(B: Boolean); overload;
    procedure ValueUInt64(Q: QWord);
    procedure Null;
    { JSON text written as it is (it must be valid) }
    procedure Raw(const JsonText: string);
    procedure RawChars(P: PChar; N: Integer);
    { the text written so far }
    function Text: string;
    procedure Clear;
  end;

{ Parses Text: a new document (free it), or EJsonError. }
function JsonParse(const Text: string;
                   Options: TJsonReaderOptions = []): TJsonDocument;
{ Is Text one valid JSON value? }
function JsonValid(const Text: string): Boolean;
{ D as JSON: the shortest text that reads back as D }
function JsonFloat(D: Double): string;
{ the elements of an array, the members of an object, for for-in }
function JsonItems(const V: TJsonValue): iterator TJsonValue;
function JsonMembers(const V: TJsonValue): iterator TJsonMember;

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

function HexValue(C: Char): Integer; inline;
begin
  case C of
    '0'..'9': Result := Ord(C) - Ord('0');
    'a'..'f': Result := Ord(C) - Ord('a') + 10;
  else
    Result := Ord(C) - Ord('A') + 10;
  end;
end;

{ The code point of the 4 hex digits at P[I] (checked before). }
function Hex4(P: PChar; I: Integer): Integer; inline;
begin
  Result := (HexValue(P[I]) shl 12) or (HexValue(P[I + 1]) shl 8) or
            (HexValue(P[I + 2]) shl 4) or HexValue(P[I + 3]);
end;

function IsHex4(P: PChar; I, Len: Integer): Boolean;
var
  K: Integer;
begin
  Result := I + 4 <= Len;
  if Result then
    for K := I to I + 3 do
      if not (P[K] in ['0'..'9', 'a'..'f', 'A'..'F']) then
        Result := False;
end;


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
            N := Hex4(FP, I + 2);
            if (N >= $D800) and (N <= $DBFF) then
            begin
              { a surrogate pair: the low half must follow }
              if (I + 12 > FLen) or (FP[I + 6] <> '\') or (FP[I + 7] <> 'u') or
                 not IsHex4(FP, I + 8, FLen) then
                Fail('lone leading surrogate in hex escape', I);
              N := Hex4(FP, I + 8);
              if (N < $DC00) or (N > $DFFF) then
                Fail('invalid surrogate pair in hex escape', I + 6);
              Inc(I, 6);
            end
            else if (N >= $DC00) and (N <= $DFFF) then
              Fail('lone trailing surrogate in hex escape', I);
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

{ The text of the string P[Start, Stop) (its escapes checked when it was
  read), decoded. }
function JsonDecode(P: PChar; Start, Stop: Integer): string;
var
  I, Len, Run, CP, Lo: Integer;
  C: Char;
  Buf: string;
begin
  SetLength(Buf, Stop - Start);
  Len := 0;
  I := Start;
  Run := I;
  while I < Stop do
  begin
    if P[I] <> '\' then
    begin
      Inc(I);
      Continue;
    end;
    AddChars(Buf, Len, P + Run, I - Run);
    C := P[I + 1];
    Inc(I, 2);
    case C of
      'b': AddChar(Buf, Len, #8);
      'f': AddChar(Buf, Len, #12);
      'n': AddChar(Buf, Len, #10);
      'r': AddChar(Buf, Len, #13);
      't': AddChar(Buf, Len, #9);
      'u':
        begin
          CP := Hex4(P, I);
          Inc(I, 4);
          if (CP >= $D800) and (CP <= $DBFF) then
          begin
            Lo := Hex4(P, I + 2);
            Inc(I, 6);
            CP := $10000 + ((CP - $D800) shl 10) + (Lo - $DC00);
          end;
          if CP < $80 then
            AddChar(Buf, Len, Chr(CP))
          else if CP < $800 then
          begin
            AddChar(Buf, Len, Chr($C0 or (CP shr 6)));
            AddChar(Buf, Len, Chr($80 or (CP and $3F)));
          end
          else if CP < $10000 then
          begin
            AddChar(Buf, Len, Chr($E0 or (CP shr 12)));
            AddChar(Buf, Len, Chr($80 or ((CP shr 6) and $3F)));
            AddChar(Buf, Len, Chr($80 or (CP and $3F)));
          end
          else
          begin
            AddChar(Buf, Len, Chr($F0 or (CP shr 18)));
            AddChar(Buf, Len, Chr($80 or ((CP shr 12) and $3F)));
            AddChar(Buf, Len, Chr($80 or ((CP shr 6) and $3F)));
            AddChar(Buf, Len, Chr($80 or (CP and $3F)));
          end;
        end;
    else
      AddChar(Buf, Len, C); { " \ / }
    end;
    Run := I;
  end;
  AddChars(Buf, Len, P + Run, Stop - Run);
  SetLength(Buf, Len);
  Result := Buf;
end;

function TJsonReader.AsString: string;
begin
  if not (FToken in [jtString, jtKey]) then
    Fail('not a string', FStart);
  if not FEscaped then
    Exit(Copy(FText, FStart + 1, FStop - FStart));
  Result := JsonDecode(FP, FStart, FStop);
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

{ --- writing --- }

function c_snprintf(buf: PChar; n: SizeInt; fmt: PChar): Integer; cdecl;
  varargs; external 'c' name 'snprintf';

function JsonFloat(D: Double): string;
var
  Buf: array[0..39] of Char;
  P: Integer;
  S: string;
begin
  if D - D <> 0 then
    raise EJsonError.Create('a number that is not finite cannot be written as JSON');
  for P := 15 to 17 do
  begin
    c_snprintf(@Buf[0], 40, '%.*g', P, D);
    if (P = 17) or (c_strtod(@Buf[0], nil) = D) then
      Break;
  end;
  S := PChar(@Buf[0]);
  if (Pos('.', S) = 0) and (Pos('e', S) = 0) then
    S := S + '.0';
  Result := S;
end;

constructor TJsonWriter.Create(Pretty: Boolean);
begin
  FPretty := Pretty;
  SetLength(FBuf, 256);
  FLen := 0;
  FDepth := 0;
end;

procedure TJsonWriter.Clear;
begin
  FLen := 0;
  FDepth := 0;
  FAfterKey := False;
end;

function TJsonWriter.Text: string;
begin
  Result := Copy(FBuf, 1, FLen);
end;

procedure TJsonWriter.RawChars(P: PChar; N: Integer);
begin
  AddChars(FBuf, FLen, P, N);
end;

procedure TJsonWriter.NewLine;
var
  I: Integer;
begin
  AddChar(FBuf, FLen, #10);
  for I := 1 to FDepth do
  begin
    AddChar(FBuf, FLen, ' ');
    AddChar(FBuf, FLen, ' ');
  end;
end;

{ the comma and line break before a value (not after a key) }
procedure TJsonWriter.BeforeValue;
begin
  if FAfterKey then
  begin
    FAfterKey := False;
    Exit;
  end;
  if FDepth > 0 then
  begin
    if FCount[FDepth - 1] > 0 then
      AddChar(FBuf, FLen, ',');
    Inc(FCount[FDepth - 1]);
    if FPretty then
      NewLine;
  end;
end;

procedure TJsonWriter.Open(C: Char);
begin
  BeforeValue;
  AddChar(FBuf, FLen, C);
  if FDepth >= Length(FCount) then
    SetLength(FCount, Length(FCount) * 2 + 16);
  FCount[FDepth] := 0;
  Inc(FDepth);
end;

procedure TJsonWriter.Close(C: Char);
var
  Had: Boolean;
begin
  if FDepth = 0 then
    raise EJsonError.Create('nothing to close');
  Had := FCount[FDepth - 1] > 0;
  Dec(FDepth);
  if FPretty and Had then
    NewLine;
  AddChar(FBuf, FLen, C);
end;

procedure TJsonWriter.BeginObject;
begin
  Open('{');
end;

procedure TJsonWriter.EndObject;
begin
  Close('}');
end;

procedure TJsonWriter.BeginArray;
begin
  Open('[');
end;

procedure TJsonWriter.EndArray;
begin
  Close(']');
end;

procedure TJsonWriter.Key(const K: string);
begin
  BeforeValue;
  AddChar(FBuf, FLen, '"');
  JsonEscapeTo(FBuf, FLen, K);
  AddChar(FBuf, FLen, '"');
  AddChar(FBuf, FLen, ':');
  if FPretty then
    AddChar(FBuf, FLen, ' ');
  FAfterKey := True;
end;

procedure TJsonWriter.Value(const S: string);
begin
  BeforeValue;
  AddChar(FBuf, FLen, '"');
  JsonEscapeTo(FBuf, FLen, S);
  AddChar(FBuf, FLen, '"');
end;

procedure TJsonWriter.Value(I: Int64);
var
  S: string;
begin
  BeforeValue;
  S := IntToStr(I);
  AddChars(FBuf, FLen, PChar(S), Length(S));
end;

procedure TJsonWriter.ValueUInt64(Q: QWord);
var
  S: string;
begin
  BeforeValue;
  S := IntToStr(Q);
  AddChars(FBuf, FLen, PChar(S), Length(S));
end;

procedure TJsonWriter.Value(D: Double);
var
  S: string;
begin
  S := JsonFloat(D);
  BeforeValue;
  AddChars(FBuf, FLen, PChar(S), Length(S));
end;

procedure TJsonWriter.Value(B: Boolean);
begin
  BeforeValue;
  if B then
    AddChars(FBuf, FLen, 'true', 4)
  else
    AddChars(FBuf, FLen, 'false', 5);
end;

procedure TJsonWriter.Null;
begin
  BeforeValue;
  AddChars(FBuf, FLen, 'null', 4);
end;

procedure TJsonWriter.Raw(const JsonText: string);
begin
  BeforeValue;
  AddChars(FBuf, FLen, PChar(JsonText), Length(JsonText));
end;

{ --- documents --- }

class operator TJsonKey.:=(const S: string): TJsonKey;
begin
  Result.Name := S;
  Result.Index := -1;
end;

class operator TJsonKey.:=(I: Integer): TJsonKey;
begin
  Result.Name := '';
  Result.Index := I;
end;

const
  KindNames: array[TJsonKind] of string = ('null', 'a boolean', 'a boolean',
    'a number', 'a string', 'an array', 'an object');

constructor TJsonDocument.Create;
begin
  FRoot := NewNode(jkNull);
end;

destructor TJsonDocument.Destroy;
begin
  FBlocks := nil;
  inherited Destroy;
end;

function TJsonDocument.NewNode(K: TJsonKind): PJsonNode;
var
  N: Integer;
begin
  N := Length(FBlocks);
  if (N = 0) or (FUsed = Length(FBlocks[N - 1])) then
  begin
    SetLength(FBlocks, N + 1);
    if N = 0 then
      SetLength(FBlocks[N], 64)
    else if Length(FBlocks[N - 1]) < 65536 then
      SetLength(FBlocks[N], Length(FBlocks[N - 1]) * 2)
    else
      SetLength(FBlocks[N], 65536);
    FUsed := 0;
    Inc(N);
  end;
  Result := @FBlocks[N - 1][FUsed];
  Inc(FUsed);
  Result^.Kind := K;
end;

function TJsonDocument.Handle(N: PJsonNode): TJsonValue;
begin
  Result.FNode := N;
  Result.FDoc := Self;
end;

function TJsonDocument.Root: TJsonValue;
begin
  Result := Handle(FRoot);
end;

procedure TJsonDocument.SetRoot(const V: TJsonValue);
begin
  FRoot := Root.Adopt(V);
end;

function TJsonDocument.NodeString(N: PJsonNode): string;
begin
  if not N^.InSource then
    Exit(N^.Str);
  if not N^.Escaped then
    Exit(Copy(FSource, N^.Start + 1, N^.Len));
  { decoded once }
  N^.Str := JsonDecode(PChar(FSource), N^.Start, N^.Start + N^.Len);
  N^.InSource := False;
  Result := N^.Str;
end;

procedure TJsonDocument.Load(const Text: string; Options: TJsonReaderOptions);
var
  R: TJsonReader;
  Frames: array of PJsonNode;
  Bases: array of Integer;
  Vals, Keys: array of PJsonNode;
  Depth, Top, I, B: Integer;
  N, PendingKey, C: PJsonNode;

  procedure Attach(X: PJsonNode);
  begin
    if Depth = 0 then
    begin
      FRoot := X;
      Exit;
    end;
    if Top >= Length(Vals) then
    begin
      SetLength(Vals, Length(Vals) * 2 + 64);
      SetLength(Keys, Length(Vals));
    end;
    Vals[Top] := X;
    Keys[Top] := PendingKey;
    Inc(Top);
  end;

  function Leaf(K: TJsonKind): PJsonNode;
  begin
    Result := NewNode(K);
    Attach(Result);
  end;

begin
  FBlocks := nil;
  FUsed := 0;
  FSource := Text;
  R := TJsonReader.Create(FSource, Options);
  Depth := 0;
  Top := 0;
  PendingKey := nil;
  try
    while R.Read do
      case R.Token of
        jtBeginObject, jtBeginArray:
          begin
            if R.Token = jtBeginObject then N := Leaf(jkObject) else N := Leaf(jkArray);
            if Depth >= Length(Frames) then
            begin
              SetLength(Frames, Length(Frames) * 2 + 16);
              SetLength(Bases, Length(Frames));
            end;
            Frames[Depth] := N;
            Bases[Depth] := Top;
            Inc(Depth);
          end;
        jtEndObject, jtEndArray:
          begin
            Dec(Depth);
            C := Frames[Depth];
            B := Bases[Depth];
            C^.Count := Top - B;
            SetLength(C^.Items, C^.Count);
            for I := 0 to C^.Count - 1 do
              C^.Items[I] := Vals[B + I];
            if C^.Kind = jkObject then
            begin
              SetLength(C^.Keys, C^.Count);
              for I := 0 to C^.Count - 1 do
                C^.Keys[I] := Keys[B + I];
            end;
            Top := B;
          end;
        jtKey:
          begin
            PendingKey := NewNode(jkString);
            PendingKey^.InSource := True;
            PendingKey^.Escaped := R.TokenEscaped;
            PendingKey^.Start := R.TokenStart;
            PendingKey^.Len := R.TokenStop - R.TokenStart;
          end;
        jtString:
          begin
            N := Leaf(jkString);
            N^.InSource := True;
            N^.Escaped := R.TokenEscaped;
            N^.Start := R.TokenStart;
            N^.Len := R.TokenStop - R.TokenStart;
          end;
        jtNumber:
          begin
            N := Leaf(jkNumber);
            N^.InSource := True;
            N^.Start := R.TokenStart;
            N^.Len := R.TokenStop - R.TokenStart;
            N^.NumKind := R.NumberKind;
            case N^.NumKind of
              jnInt64: N^.Int := R.AsInt64;
              jnUInt64: N^.Int := Int64(R.AsUInt64);
            end;
            N^.Dbl := R.AsDouble;
          end;
        jtTrue: Leaf(jkTrue);
        jtFalse: Leaf(jkFalse);
        jtNull: Leaf(jkNull);
      end;
  finally
    R.Free;
  end;
end;

function TJsonDocument.CopyNode(Src: TJsonDocument; N: PJsonNode): PJsonNode;
var
  I: Integer;
begin
  Result := NewNode(N^.Kind);
  Result^.NumKind := N^.NumKind;
  Result^.Int := N^.Int;
  Result^.Dbl := N^.Dbl;
  case N^.Kind of
    jkString: Result^.Str := Src.NodeString(N);
    jkNumber:
      if N^.InSource then
      begin
        { (its text as written) }
        Result^.Str := Copy(Src.FSource, N^.Start + 1, N^.Len);
      end
      else
        Result^.Str := N^.Str;
    jkArray, jkObject:
      begin
        Result^.Count := N^.Count;
        SetLength(Result^.Items, N^.Count);
        for I := 0 to N^.Count - 1 do
          Result^.Items[I] := CopyNode(Src, N^.Items[I]);
        if N^.Kind = jkObject then
        begin
          SetLength(Result^.Keys, N^.Count);
          for I := 0 to N^.Count - 1 do
            Result^.Keys[I] := CopyNode(Src, N^.Keys[I]);
        end;
      end;
  end;
end;

function TJsonDocument.NewObject: TJsonValue;
begin
  Result := Handle(NewNode(jkObject));
end;

function TJsonDocument.NewArray: TJsonValue;
begin
  Result := Handle(NewNode(jkArray));
end;

function TJsonDocument.NewString(const S: string): TJsonValue;
var
  N: PJsonNode;
begin
  N := NewNode(jkString);
  N^.Str := S;
  Result := Handle(N);
end;

function TJsonDocument.NewInteger(I: Int64): TJsonValue;
var
  N: PJsonNode;
begin
  N := NewNode(jkNumber);
  N^.NumKind := jnInt64;
  N^.Int := I;
  N^.Dbl := I;
  Result := Handle(N);
end;

function TJsonDocument.NewNumber(D: Double): TJsonValue;
var
  N: PJsonNode;
begin
  N := NewNode(jkNumber);
  N^.NumKind := jnDouble;
  N^.Dbl := D;
  Result := Handle(N);
end;

function TJsonDocument.NewBoolean(B: Boolean): TJsonValue;
begin
  if B then
    Result := Handle(NewNode(jkTrue))
  else
    Result := Handle(NewNode(jkFalse));
end;

function TJsonDocument.NewNull: TJsonValue;
begin
  Result := Handle(NewNode(jkNull));
end;

function TJsonDocument.ToJson(Pretty: Boolean): string;
begin
  Result := Root.ToJson(Pretty);
end;

function JsonParse(const Text: string; Options: TJsonReaderOptions): TJsonDocument;
var
  D: TJsonDocument;
begin
  D := TJsonDocument.Create;
  try
    D.Load(Text, Options);
  except
    D.Free;
    raise;
  end;
  Result := D;
end;

function JsonValid(const Text: string): Boolean;
var
  R: TJsonReader;
begin
  R := TJsonReader.Create(Text);
  try
    try
      while R.Read do
        ;
      Result := True;
    except
      on EJsonError do
        Result := False;
    end;
  finally
    R.Free;
  end;
end;

{ --- values --- }

procedure TJsonValue.Need(K: TJsonKind; const What: string);
var
  Found: string;
begin
  if FNode = nil then
    raise EJsonError.Create('expected ' + What + ', but the value is not there');
  if (FNode^.Kind = K) or ((K = jkTrue) and (FNode^.Kind = jkFalse)) then
    Exit;
  Found := KindNames[FNode^.Kind];
  raise EJsonError.Create('expected ' + What + ', found ' + Found);
end;

function TJsonValue.Exists: Boolean;
begin
  Result := FNode <> nil;
end;

function TJsonValue.Kind: TJsonKind;
begin
  if FNode = nil then
    raise EJsonError.Create('the value is not there');
  Result := FNode^.Kind;
end;

function TJsonValue.IsNull: Boolean;
begin
  Result := (FNode <> nil) and (FNode^.Kind = jkNull);
end;

function TJsonValue.IsBoolean: Boolean;
begin
  Result := (FNode <> nil) and (FNode^.Kind in [jkTrue, jkFalse]);
end;

function TJsonValue.IsNumber: Boolean;
begin
  Result := (FNode <> nil) and (FNode^.Kind = jkNumber);
end;

function TJsonValue.IsInteger: Boolean;
begin
  Result := (FNode <> nil) and (FNode^.Kind = jkNumber) and
            (FNode^.NumKind <> jnDouble);
end;

function TJsonValue.IsString: Boolean;
begin
  Result := (FNode <> nil) and (FNode^.Kind = jkString);
end;

function TJsonValue.IsArray: Boolean;
begin
  Result := (FNode <> nil) and (FNode^.Kind = jkArray);
end;

function TJsonValue.IsObject: Boolean;
begin
  Result := (FNode <> nil) and (FNode^.Kind = jkObject);
end;

function TJsonValue.AsString: string;
begin
  Need(jkString, 'a string');
  Result := FDoc.NodeString(FNode);
end;

function TJsonValue.AsInt64: Int64;
begin
  Need(jkNumber, 'a number');
  if FNode^.NumKind <> jnInt64 then
    raise EJsonError.Create('expected an Int64, found ' +
                            FDoc.Handle(FNode).ToJson);
  Result := FNode^.Int;
end;

function TJsonValue.AsUInt64: QWord;
begin
  Need(jkNumber, 'a number');
  if (FNode^.NumKind = jnUInt64) or
     ((FNode^.NumKind = jnInt64) and (FNode^.Int >= 0)) then
    Result := QWord(FNode^.Int)
  else
    raise EJsonError.Create('expected a UInt64, found ' +
                            FDoc.Handle(FNode).ToJson);
end;

function TJsonValue.AsDouble: Double;
begin
  Need(jkNumber, 'a number');
  Result := FNode^.Dbl;
end;

function TJsonValue.AsBoolean: Boolean;
begin
  Need(jkTrue, 'a boolean');
  Result := FNode^.Kind = jkTrue;
end;

function TJsonValue.AsStringDef(const Default: string): string;
begin
  if IsString then Result := AsString else Result := Default;
end;

function TJsonValue.AsInt64Def(Default: Int64): Int64;
begin
  if IsNumber and (FNode^.NumKind = jnInt64) then
    Result := FNode^.Int
  else
    Result := Default;
end;

function TJsonValue.AsDoubleDef(Default: Double): Double;
begin
  if IsNumber then Result := FNode^.Dbl else Result := Default;
end;

function TJsonValue.AsBooleanDef(Default: Boolean): Boolean;
begin
  if IsBoolean then Result := FNode^.Kind = jkTrue else Result := Default;
end;

function TJsonValue.Count: Integer;
begin
  if (FNode <> nil) and (FNode^.Kind in [jkArray, jkObject]) then
    Result := FNode^.Count
  else
    Result := 0;
end;

{ Is key node K the string S? (no decoding when it has no escapes) }
function KeyIs(D: TJsonDocument; K: PJsonNode; const S: string): Boolean;
var
  P, Q: PChar;
  I, N: Integer;
begin
  if not K^.InSource or K^.Escaped then
    Exit(D.NodeString(K) = S);
  N := K^.Len;
  if N <> Length(S) then
    Exit(False);
  P := PChar(D.FSource) + K^.Start;
  Q := PChar(S);
  I := 0;
  while I + 8 <= N do
  begin
    if PQWord(P + I)^ <> PQWord(Q + I)^ then
      Exit(False);
    Inc(I, 8);
  end;
  while I < N do
  begin
    if P[I] <> Q[I] then
      Exit(False);
    Inc(I);
  end;
  Result := True;
end;

function IndexOfKey(D: TJsonDocument; N: PJsonNode; const Key: string): Integer;
var
  I: Integer;
begin
  for I := 0 to N^.Count - 1 do
    if KeyIs(D, N^.Keys[I], Key) then
      Exit(I);
  Result := -1;
end;

function TJsonValue.Get(const Key: string): TJsonValue;
var
  I: Integer;
begin
  Result.FDoc := FDoc;
  Result.FNode := nil;
  if (FNode = nil) or (FNode^.Kind <> jkObject) then
    Exit;
  I := IndexOfKey(FDoc, FNode, Key);
  if I >= 0 then
    Result.FNode := FNode^.Items[I];
end;

function TJsonValue.TryGet(const Key: string; out V: TJsonValue): Boolean;
begin
  V := Get(Key);
  Result := V.FNode <> nil;
end;

function TJsonValue.Has(const Key: string): Boolean;
begin
  Result := Get(Key).FNode <> nil;
end;

function TJsonValue.At(I: Integer): TJsonValue;
begin
  Result.FDoc := FDoc;
  Result.FNode := nil;
  if (FNode <> nil) and (FNode^.Kind in [jkArray, jkObject]) and (I >= 0) and
     (I < FNode^.Count) then
    Result.FNode := FNode^.Items[I];
end;

function TJsonValue.KeyAt(I: Integer): string;
begin
  Need(jkObject, 'an object');
  if (I < 0) or (I >= FNode^.Count) then
    raise EJsonError.Create('no member ' + IntToStr(I));
  Result := FDoc.NodeString(FNode^.Keys[I]);
end;

function TJsonValue.GetItem(const K: TJsonKey): TJsonValue;
begin
  if K.Index >= 0 then
    Result := At(K.Index)
  else
    Result := Get(K.Name);
end;

function TJsonValue.Path(const Pointer: string): TJsonValue;
var
  I, J, Idx: Integer;
  Part: string;
  Ok: Boolean;
begin
  Result := Self;
  if Pointer = '' then
    Exit;
  if Pointer[1] <> '/' then
    raise EJsonError.Create('a JSON Pointer starts with ''/'': ''' + Pointer + '''');
  I := 2;
  while Result.FNode <> nil do
  begin
    J := I;
    while (J <= Length(Pointer)) and (Pointer[J] <> '/') do
      Inc(J);
    Part := Copy(Pointer, I, J - I);
    { ~1 is '/', ~0 is '~' }
    Part := StringReplace(StringReplace(Part, '~1', '/', [rfReplaceAll]),
                          '~0', '~', [rfReplaceAll]);
    if Result.FNode^.Kind = jkArray then
    begin
      Ok := (Part <> '') and ((Part = '0') or (Part[1] <> '0'));
      Idx := 0;
      for J := 1 to Length(Part) do
        if Ok and (Part[J] in ['0'..'9']) and (Idx < 100000000) then
          Idx := Idx * 10 + Ord(Part[J]) - Ord('0')
        else
          Ok := False;
      if Ok then
        Result := Result.At(Idx)
      else
        Result.FNode := nil;
    end
    else
      Result := Result.Get(Part);
    J := I;
    while (J <= Length(Pointer)) and (Pointer[J] <> '/') do
      Inc(J);
    if J > Length(Pointer) then
      Exit;
    I := J + 1;
  end;
end;

{ --- changes --- }

function TJsonValue.NewChild(K: TJsonKind): PJsonNode;
begin
  if FNode = nil then
    raise EJsonError.Create('the value is not there');
  Result := FDoc.NewNode(K);
end;

{ V's node in this document (copied if it is another document's) }
function TJsonValue.Adopt(const V: TJsonValue): PJsonNode;
begin
  if V.FNode = nil then
    raise EJsonError.Create('the value is not there');
  if V.FDoc = FDoc then
    Result := V.FNode
  else
    Result := FDoc.CopyNode(V.FDoc, V.FNode);
end;

procedure Grow(N: PJsonNode);
var
  Cap: Integer;
begin
  Cap := Length(N^.Items);
  if N^.Count < Cap then
    Exit;
  Cap := Cap * 2 + 4;
  SetLength(N^.Items, Cap);
  if N^.Kind = jkObject then
    SetLength(N^.Keys, Cap);
end;

procedure TJsonValue.AddNode(N: PJsonNode);
begin
  Need(jkArray, 'an array');
  Grow(FNode);
  FNode^.Items[FNode^.Count] := N;
  Inc(FNode^.Count);
end;

procedure TJsonValue.PutNode(const Key: string; N: PJsonNode);
var
  I: Integer;
  K: PJsonNode;
begin
  Need(jkObject, 'an object');
  I := IndexOfKey(FDoc, FNode, Key);
  if I >= 0 then
  begin
    FNode^.Items[I] := N;
    Exit;
  end;
  Grow(FNode);
  K := FDoc.NewNode(jkString);
  K^.Str := Key;
  FNode^.Keys[FNode^.Count] := K;
  FNode^.Items[FNode^.Count] := N;
  Inc(FNode^.Count);
end;

procedure TJsonValue.Put(const Key: string; const S: string);
begin
  PutNode(Key, FDoc.NewString(S).FNode);
end;

procedure TJsonValue.Put(const Key: string; I: Int64);
begin
  PutNode(Key, FDoc.NewInteger(I).FNode);
end;

procedure TJsonValue.Put(const Key: string; D: Double);
begin
  PutNode(Key, FDoc.NewNumber(D).FNode);
end;

procedure TJsonValue.Put(const Key: string; B: Boolean);
begin
  PutNode(Key, FDoc.NewBoolean(B).FNode);
end;

procedure TJsonValue.Put(const Key: string; const V: TJsonValue);
begin
  PutNode(Key, Adopt(V));
end;

procedure TJsonValue.PutNull(const Key: string);
begin
  PutNode(Key, NewChild(jkNull));
end;

function TJsonValue.PutObject(const Key: string): TJsonValue;
begin
  Result := FDoc.Handle(NewChild(jkObject));
  PutNode(Key, Result.FNode);
end;

function TJsonValue.PutArray(const Key: string): TJsonValue;
begin
  Result := FDoc.Handle(NewChild(jkArray));
  PutNode(Key, Result.FNode);
end;

procedure TJsonValue.Add(const S: string);
begin
  AddNode(FDoc.NewString(S).FNode);
end;

procedure TJsonValue.Add(I: Int64);
begin
  AddNode(FDoc.NewInteger(I).FNode);
end;

procedure TJsonValue.Add(D: Double);
begin
  AddNode(FDoc.NewNumber(D).FNode);
end;

procedure TJsonValue.Add(B: Boolean);
begin
  AddNode(FDoc.NewBoolean(B).FNode);
end;

procedure TJsonValue.Add(const V: TJsonValue);
begin
  AddNode(Adopt(V));
end;

procedure TJsonValue.AddNull;
begin
  AddNode(NewChild(jkNull));
end;

function TJsonValue.AddObject: TJsonValue;
begin
  Result := FDoc.Handle(NewChild(jkObject));
  AddNode(Result.FNode);
end;

function TJsonValue.AddArray: TJsonValue;
begin
  Result := FDoc.Handle(NewChild(jkArray));
  AddNode(Result.FNode);
end;

function TJsonValue.Delete(const Key: string): Boolean;
var
  I, J: Integer;
begin
  Need(jkObject, 'an object');
  I := IndexOfKey(FDoc, FNode, Key);
  Result := I >= 0;
  if not Result then
    Exit;
  for J := I to FNode^.Count - 2 do
  begin
    FNode^.Items[J] := FNode^.Items[J + 1];
    FNode^.Keys[J] := FNode^.Keys[J + 1];
  end;
  Dec(FNode^.Count);
end;

procedure TJsonValue.Remove(I: Integer);
var
  J: Integer;
begin
  Need(jkArray, 'an array');
  if (I < 0) or (I >= FNode^.Count) then
    raise EJsonError.Create('no element ' + IntToStr(I));
  for J := I to FNode^.Count - 2 do
    FNode^.Items[J] := FNode^.Items[J + 1];
  Dec(FNode^.Count);
end;

procedure TJsonValue.Clear;
begin
  if (FNode <> nil) and (FNode^.Kind in [jkArray, jkObject]) then
    FNode^.Count := 0;
end;

{ --- writing values --- }

procedure WriteNode(W: TJsonWriter; D: TJsonDocument; N: PJsonNode);
var
  I: Integer;
  K: PJsonNode;
begin
  case N^.Kind of
    jkNull: W.Null;
    jkTrue: W.Value(True);
    jkFalse: W.Value(False);
    jkNumber:
      if N^.InSource then
      begin
        W.BeforeValue; { (as written) }
        W.RawChars(PChar(D.FSource) + N^.Start, N^.Len);
      end
      else if N^.Str <> '' then
        W.Raw(N^.Str)
      else
        case N^.NumKind of
          jnInt64: W.Value(N^.Int);
          jnUInt64: W.ValueUInt64(QWord(N^.Int));
        else
          W.Value(N^.Dbl);
        end;
    jkString:
      if N^.InSource then
      begin
        { (the source's text: valid JSON, escapes and all) }
        W.BeforeValue;
        AddChar(W.FBuf, W.FLen, '"');
        W.RawChars(PChar(D.FSource) + N^.Start, N^.Len);
        AddChar(W.FBuf, W.FLen, '"');
      end
      else
        W.Value(N^.Str);
    jkArray:
      begin
        W.BeginArray;
        for I := 0 to N^.Count - 1 do
          WriteNode(W, D, N^.Items[I]);
        W.EndArray;
      end;
    jkObject:
      begin
        W.BeginObject;
        for I := 0 to N^.Count - 1 do
        begin
          K := N^.Keys[I];
          if K^.InSource then
          begin
            W.BeforeValue;
            AddChar(W.FBuf, W.FLen, '"');
            W.RawChars(PChar(D.FSource) + K^.Start, K^.Len);
            AddChar(W.FBuf, W.FLen, '"');
            AddChar(W.FBuf, W.FLen, ':');
            if W.FPretty then
              AddChar(W.FBuf, W.FLen, ' ');
            W.FAfterKey := True;
          end
          else
            W.Key(K^.Str);
          WriteNode(W, D, N^.Items[I]);
        end;
        W.EndObject;
      end;
  end;
end;

procedure TJsonValue.WriteTo(W: TJsonWriter);
begin
  if FNode = nil then
    raise EJsonError.Create('the value is not there');
  WriteNode(W, FDoc, FNode);
end;

function TJsonValue.ToJson(Pretty: Boolean): string;
var
  W: TJsonWriter;
begin
  W := TJsonWriter.Create(Pretty);
  try
    WriteTo(W);
    Result := W.Text;
  finally
    W.Free;
  end;
end;

function JsonItems(const V: TJsonValue): iterator TJsonValue;
var
  I: Integer;
begin
  for I := 0 to V.Count - 1 do
    yield V.At(I);
end;

function JsonMembers(const V: TJsonValue): iterator TJsonMember;
var
  I: Integer;
  M: TJsonMember;
begin
  if not V.IsObject then
    Exit;
  for I := 0 to V.Count - 1 do
  begin
    M.Key := V.KeyAt(I);
    M.Value := V.At(I);
    yield M;
  end;
end;

end.
