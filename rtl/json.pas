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
  powers of ten up to 22, by the Eisel-Lemire algorithm for up to 19
  digits, through the C library's strtod for more).

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
    { the current number's parts, taken while reading it }
    FNumMant: QWord;
    FNumExp, FNumDigits: Integer;
    FNumNeg, FNumIsInt, FNumBig: Boolean;
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
    { a reader of the value at Text[Start, Start + Len) (0-based offsets;
      positions stay those of Text) }
    constructor CreateRange(const Text: string; Start, Len: Integer;
                            Options: TJsonReaderOptions = []);
    { Moves to the next token; false at the end of the text (Token jtEnd),
      after one whole value. }
    function Read: Boolean;
    { Passes over the current value: after a jtBeginObject or jtBeginArray
      token, to its end (its brackets matched and its strings closed, nothing
      more is checked: which is fast);
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
    { raises 'expected What, found <the current token>' }
    procedure Expected(const What: string);
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
    { an object's lookup table (open addressing, member index + 1, 0 empty;
      a power of 2 of at least twice the members), made when a big object is
      first searched; Indexed members are in it, so a table that does not
      cover Count is stale }
    Slots: array of Integer;
    Indexed: Integer;
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

  { --- lazy access ---

    JsonGet finds one value in a JSON text by its path, reading only what
    comes before it on the way (the members it passes are skipped: their
    brackets are matched and their strings closed, nothing more is
    checked). The value is a
    TJsonLazy: where it is in the text, read when asked. }

  TJsonLazy = record
  private
    FText: string;
    FStart, FLen: Integer;
    function Reader: TJsonReader;
  public
    function Exists: Boolean;
    function Kind: TJsonKind;
    { its JSON text }
    function Raw: string;
    property Text: string read FText;
    property Start: Integer read FStart;
    property Len: Integer read FLen;
    function IsNull: Boolean;
    function AsString: string;
    function AsInt64: Int64;
    function AsUInt64: QWord;
    function AsDouble: Double;
    function AsBoolean: Boolean;
    { a value inside this one }
    function Get(const Path: array of TJsonKey): TJsonLazy;
    function GetPath(const Pointer: string): TJsonLazy;
    { the value as a document (free it) }
    function Parse: TJsonDocument;
  end;

  TJsonLazyMember = record
    Key: string;
    Value: TJsonLazy;
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
{ The value at Path in Text: JsonGet(S, ['users', 0, 'name']) (not Exists
  when there is none; EJsonError for bad JSON on the way). }
function JsonGet(const Text: string; const Path: array of TJsonKey): TJsonLazy;
{ the same, by a JSON Pointer: JsonGetPath(S, '/users/0/name') }
function JsonGetPath(const Text, Pointer: string): TJsonLazy;
{ the whole text as a lazy value }
function JsonLazy(const Text: string): TJsonLazy;
{ the elements of the array Text (or L) is, the members of the object, read
  as they are asked for }
function JsonArrayItems(const Text: string): iterator TJsonLazy; overload;
function JsonArrayItems(const L: TJsonLazy): iterator TJsonLazy; overload;
function JsonObjectItems(const Text: string): iterator TJsonLazyMember; overload;
function JsonObjectItems(const L: TJsonLazy): iterator TJsonLazyMember; overload;
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
  { 16 bytes of text at once (SIMD: SSE2 on x86-64, NEON on AArch64; on a
  target without vectors LLVM makes them scalar code, slower than the
  8-byte scans they replace) }
  TBytes16 = vector[16] of Byte;

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

{$I json_pow5.inc}

type
  PDouble = ^Double;

{ the high 64 bits of A * B (Lo: the low ones) }
function Mul128(A, B: QWord; out Lo: QWord): QWord; inline;
var
  ALo, AHi, BLo, BHi, P0, P1, P2, P3, Mid: QWord;
begin
  ALo := A and $FFFFFFFF;
  AHi := A shr 32;
  BLo := B and $FFFFFFFF;
  BHi := B shr 32;
  P0 := ALo * BLo;
  P1 := ALo * BHi;
  P2 := AHi * BLo;
  P3 := AHi * BHi;
  Mid := P1 + (P0 shr 32) + (P2 and $FFFFFFFF);
  Lo := (Mid shl 32) or (P0 and $FFFFFFFF);
  Result := P3 + (Mid shr 32) + (P2 shr 32);
end;

{ the leading zero bits of W (W <> 0) }
function LeadingZeros(W: QWord): Integer;
begin
  Result := 0;
  if W shr 32 = 0 then begin Inc(Result, 32); W := W shl 32; end;
  if W shr 48 = 0 then begin Inc(Result, 16); W := W shl 16; end;
  if W shr 56 = 0 then begin Inc(Result, 8); W := W shl 8; end;
  if W shr 60 = 0 then begin Inc(Result, 4); W := W shl 4; end;
  if W shr 62 = 0 then begin Inc(Result, 2); W := W shl 2; end;
  if W shr 63 = 0 then Inc(Result);
end;

function DoubleOfBits(Bits: QWord): Double; inline;
begin
  Result := PDouble(@Bits)^;
end;

{ The double nearest to W * 10^Q (W > 0, at most 19 digits): the
  Eisel-Lemire algorithm (Lemire, "Number parsing at a gigabyte per
  second", 2021; with the 128-bit product it needs no fallback for
  doubles: Mushtak and Lemire, 2023), after fast_float's compute_float. }
function EiselLemire(W: QWord; Q: Integer): Double;
const
  Infinite = QWord($7FF0000000000000);
var
  Lz, Index, UpperBit, Shift, Power2, X: Integer;
  Hi, Lo, Hi2, Lo2, Mant: QWord;
begin
  if Q < Pow5Smallest then
    Exit(0.0);
  if Q > Pow5Largest then
    Exit(DoubleOfBits(Infinite));
  Lz := LeadingZeros(W);
  W := W shl Lz;
  Index := 2 * (Q - Pow5Smallest);
  Hi := Mul128(W, Pow5Table[Index], Lo);
  if Hi and $1FF = $1FF then
  begin
    Hi2 := Mul128(W, Pow5Table[Index + 1], Lo2);
    Lo := Lo + Hi2;
    if Hi2 > Lo then
      Inc(Hi);
  end;
  UpperBit := Integer(Hi shr 63);
  Shift := UpperBit + 9;
  Mant := Hi shr Shift;
  { floor(Q * log2(10)) + 63, as fast_float's power(q) }
  X := 217706 * Q;
  if X >= 0 then
    X := X shr 16
  else
    X := -((-X + 65535) shr 16);
  Power2 := X + 63 + UpperBit - Lz + 1023;
  if Power2 <= 0 then
  begin
    { a subnormal }
    if -Power2 + 1 >= 64 then
      Exit(0.0);
    Mant := Mant shr (-Power2 + 1);
    Mant := Mant + (Mant and 1);
    Mant := Mant shr 1;
    if Mant < QWord(1) shl 52 then
      Power2 := 0
    else
      Power2 := 1;
    Exit(DoubleOfBits(Mant or (QWord(Power2) shl 52)));
  end;
  { exactly halfway: round to even }
  if (Lo <= 1) and (Q >= -4) and (Q <= 23) and (Mant and 3 = 1) and
     (Mant shl Shift = Hi) then
    Mant := Mant and not QWord(1);
  Mant := Mant + (Mant and 1);
  Mant := Mant shr 1;
  if Mant >= QWord(2) shl 52 then
  begin
    Mant := QWord(1) shl 52;
    Inc(Power2);
  end;
  Mant := Mant and not (QWord(1) shl 52);
  if Power2 >= $7FF then
    Exit(DoubleOfBits(Infinite));
  Result := DoubleOfBits(Mant or (QWord(Power2) shl 52));
end;

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
    { ASCII runs: 16 bytes at a time, then 8 }
    if (I + 16 <= Len) and
       (VectorMask(VectorLoad(TBytes16, P + I) >= 128) = 0) then
    begin
      Inc(I, 16);
      Continue;
    end;
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
  W, M: QWord;
  V: TBytes16;
  C: Char;
begin
  P := PChar(S);
  N := Length(S);
  I := 0;
  Run := 0; { the start of the bytes not yet copied }
  while I < N do
  begin
    { 16 bytes at a time, to the next one to escape }
    if I + 16 <= N then
    begin
      V := VectorLoad(TBytes16, P + I);
      M := VectorMask((V = Ord('"')) or (V = Ord('\')) or (V < 32));
      if M = 0 then
      begin
        Inc(I, 16);
        Continue;
      end;
      Inc(I, CountTrailingZeros(M));
    end
    else if I + 8 <= N then
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

constructor TJsonReader.CreateRange(const Text: string; Start, Len: Integer;
                                    Options: TJsonReaderOptions);
begin
  Create(Text, Options);
  if Start < 0 then
    Start := 0;
  if Start + Len > Length(FText) then
    Len := Length(FText) - Start;
  FPos := Start;
  FLen := Start + Len;
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

const
  TokenKinds: array[TJsonToken] of string = ('nothing', 'an object',
    'the end of an object', 'an array', 'the end of an array', 'a key',
    'a string', 'a number', 'a boolean', 'a boolean', 'null', 'the end');

{ the current token is not What: an error at it }
procedure TJsonReader.Expected(const What: string);
var
  At: Integer;
begin
  At := FStart;
  if FToken in [jtString, jtKey] then
    Dec(At); { (its quote) }
  Fail('expected ' + What + ', found ' + TokenKinds[FToken], At);
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
      if (FPos + 8 <= FLen) and (PQWord(FP + FPos)^ = Spaces8) then
        Inc(FPos, 8)
      else
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
    { 8 bytes at a time while none ends the string or needs a look (16 at
      a time with vectors measured slower here: most strings are short) }
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

{ A number: its grammar checked, and its digits taken on the way (up to
  19 significant ones in FMant; FBig past them), for ParseNumber. }
procedure TJsonReader.ReadNumberToken;
var
  I, Digits, Frac, ExpVal: Integer;
  M: QWord;
  D: Cardinal;
  Big, ExpNeg: Boolean;
begin
  FStart := FPos;
  I := FPos;
  FNumNeg := FP[I] = '-';
  if FNumNeg then
    Inc(I);
  if I >= FLen then
    Fail('EOF while parsing a value', I);
  M := 0;
  Digits := 0;
  Frac := 0;
  Big := False;
  FNumIsInt := True;
  if FP[I] = '0' then
  begin
    Inc(I);
    if (I < FLen) and (Cardinal(Ord(FP[I]) - Ord('0')) < 10) then
      Fail('invalid number', I);
  end
  else if Cardinal(Ord(FP[I]) - Ord('1')) < 9 then
    while I < FLen do
    begin
      D := Cardinal(Ord(FP[I]) - Ord('0'));
      if D >= 10 then
        Break;
      if Digits < 19 then
        M := M * 10 + D
      else
        Big := True;
      Inc(Digits);
      Inc(I);
    end
  else
    Fail('invalid number', I);
  if (I < FLen) and (FP[I] = '.') then
  begin
    FNumIsInt := False;
    Inc(I);
    if (I >= FLen) or (Cardinal(Ord(FP[I]) - Ord('0')) >= 10) then
      Fail('invalid number', I);
    while I < FLen do
    begin
      D := Cardinal(Ord(FP[I]) - Ord('0'));
      if D >= 10 then
        Break;
      if Digits < 19 then
      begin
        M := M * 10 + D;
        Inc(Frac);
        if M <> 0 then
          Inc(Digits); { (leading zeros are not significant) }
      end
      else
        Big := True;
      Inc(I);
    end;
  end;
  FNumExp := 0;
  if (I < FLen) and ((FP[I] = 'e') or (FP[I] = 'E')) then
  begin
    FNumIsInt := False;
    Inc(I);
    ExpNeg := False;
    if (I < FLen) and ((FP[I] = '+') or (FP[I] = '-')) then
    begin
      ExpNeg := FP[I] = '-';
      Inc(I);
    end;
    if (I >= FLen) or (Cardinal(Ord(FP[I]) - Ord('0')) >= 10) then
      Fail('invalid number', I);
    ExpVal := 0;
    while I < FLen do
    begin
      D := Cardinal(Ord(FP[I]) - Ord('0'));
      if D >= 10 then
        Break;
      if ExpVal < 100000 then
        ExpVal := ExpVal * 10 + Integer(D);
      Inc(I);
    end;
    if ExpNeg then FNumExp := -ExpVal else FNumExp := ExpVal;
  end;
  FNumExp := FNumExp - Frac;
  FNumMant := M;
  FNumDigits := Digits;
  FNumBig := Big;
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

{ Over the rest of the container just opened: its brackets matched and
  its strings closed, nothing else. 16 bytes at a time: a mask of the
  quotes, backslashes and brackets, whose bits are taken in order (a
  backslash in a string skips the byte after it, brackets in strings do
  not count). }
procedure TJsonReader.SkipContainer;
var
  Level, I, P, Skip: Integer;
  V: TBytes16;
  M: QWord;
  C: Char;
  InString: Boolean;
  { the closing bracket each open one wants: Opens[1] is the container's }
  Opens: array of Char;

  { the byte at P: false when the container ends there }
  function Take(P: Integer): Boolean;
  begin
    Result := True;
    if P < Skip then
      Exit; { (escaped) }
    C := FP[P];
    if InString then
    begin
      if C = '\' then
        Skip := P + 2
      else if C = '"' then
        InString := False;
      Exit;
    end;
    case C of
      '"': InString := True;
      '{', '[':
        begin
          Inc(Level);
          if Level >= Length(Opens) then
            SetLength(Opens, Length(Opens) * 2 + 16);
          if C = '{' then Opens[Level] := '}' else Opens[Level] := ']';
        end;
      '}', ']':
        begin
          if C <> Opens[Level] then
            Fail('expected '','' or ''' + Opens[Level] + '''', P);
          Dec(Level);
          if Level = 0 then
          begin
            FStart := P;
            FPos := P + 1;
            if C = '}' then FToken := jtEndObject else FToken := jtEndArray;
            Dec(FDepth);
            if FDepth = 0 then FState := rsDone else FState := rsAfterValue;
            Result := False;
          end;
        end;
    end;
  end;

begin
  Level := 1;
  SetLength(Opens, 16);
  if FStack[FDepth - 1] then Opens[1] := '}' else Opens[1] := ']';
  I := FPos;
  Skip := 0;
  InString := False;
  while I + 16 <= FLen do
  begin
    V := VectorLoad(TBytes16, FP + I);
    M := VectorMask((V = Ord('"')) or (V = Ord('\')) or (V = Ord('{')) or
                    (V = Ord('}')) or (V = Ord('[')) or (V = Ord(']')));
    while M <> 0 do
    begin
      P := I + CountTrailingZeros(M);
      if not Take(P) then
        Exit;
      M := M and (M - 1);
    end;
    Inc(I, 16);
  end;
  while I < FLen do
  begin
    if FP[I] in ['"', '\', '{', '}', '[', ']'] then
      if not Take(I) then
        Exit;
    Inc(I);
  end;
  if InString then
    Fail('EOF while parsing a string', FLen);
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
  M: QWord;
begin
  SetLength(Buf, Stop - Start);
  Len := 0;
  I := Start;
  Run := I;
  while I < Stop do
  begin
    if P[I] <> '\' then
    begin
      { on to the next backslash, 16 bytes at a time }
      if I + 16 <= Stop then
      begin
        M := VectorMask(VectorLoad(TBytes16, P + I) = Ord('\'));
        if M = 0 then
          Inc(I, 16)
        else
          Inc(I, CountTrailingZeros(M));
      end
      else
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
    Expected('a string');
  if not FEscaped then
    Exit(Copy(FText, FStart + 1, FStop - FStart));
  Result := JsonDecode(FP, FStart, FStop);
end;

{ Is the escaped text P[Start, Stop), decoded, the string S? Compared as it is
  decoded: no string is made. }
function EscapedEquals(P: PChar; Start, Stop: Integer; const S: string): Boolean;
var
  I, O, CP, Lo, N, J, T: Integer;
  Ok: Boolean;
  Bytes: array[0..3] of Byte;
  K: Integer;
  M: QWord;
  Q: PChar;
begin
  I := Start;
  O := 0;
  N := Length(S);
  Q := PChar(S);
  Ok := True;
  while Ok and (I < Stop) do
  begin
    K := 0;
    if P[I] <> '\' then
    begin
      { a run to the next backslash (16 bytes at a time), compared whole }
      J := I;
      while J < Stop do
      begin
        if J + 16 <= Stop then
        begin
          M := VectorMask(VectorLoad(TBytes16, P + J) = Ord('\'));
          if M = 0 then
            Inc(J, 16)
          else
          begin
            Inc(J, CountTrailingZeros(M));
            Break;
          end;
        end
        else if P[J] = '\' then
          Break
        else
          Inc(J);
      end;
      if O + (J - I) > N then
        Ok := False
      else
      begin
        T := 0;
        while Ok and (T + 8 <= J - I) do
        begin
          if PQWord(P + I + T)^ <> PQWord(Q + O + T)^ then
            Ok := False;
          Inc(T, 8);
        end;
        while Ok and (T < J - I) do
        begin
          if P[I + T] <> Q[O + T] then
            Ok := False;
          Inc(T);
        end;
      end;
      O := O + (J - I);
      I := J;
      Continue;
    end
    else
    begin
      case P[I + 1] of
        'b': Bytes[0] := 8;
        'f': Bytes[0] := 12;
        'n': Bytes[0] := 10;
        'r': Bytes[0] := 13;
        't': Bytes[0] := 9;
        'u': Bytes[0] := 0;
      else
        Bytes[0] := Ord(P[I + 1]);   { " \ / }
      end;
      K := 1;
      if P[I + 1] = 'u' then
      begin
        CP := Hex4(P, I + 2);
        Inc(I, 6);
        if (CP >= $D800) and (CP <= $DBFF) then
        begin
          Lo := Hex4(P, I + 2);
          Inc(I, 6);
          CP := $10000 + ((CP - $D800) shl 10) + (Lo - $DC00);
        end;
        if CP < $80 then
          Bytes[0] := CP
        else if CP < $800 then
        begin
          Bytes[0] := $C0 or (CP shr 6);
          Bytes[1] := $80 or (CP and $3F);
          K := 2;
        end
        else if CP < $10000 then
        begin
          Bytes[0] := $E0 or (CP shr 12);
          Bytes[1] := $80 or ((CP shr 6) and $3F);
          Bytes[2] := $80 or (CP and $3F);
          K := 3;
        end
        else
        begin
          Bytes[0] := $F0 or (CP shr 18);
          Bytes[1] := $80 or ((CP shr 12) and $3F);
          Bytes[2] := $80 or ((CP shr 6) and $3F);
          Bytes[3] := $80 or (CP and $3F);
          K := 4;
        end;
      end
      else
        Inc(I, 2);
    end;
    if O + K > N then
      Ok := False
    else
      for CP := 0 to K - 1 do
      begin
        if Ord(S[O + 1 + CP]) <> Bytes[CP] then
          Ok := False;
      end;
    O := O + K;
  end;
  Result := Ok and (O = N);
end;

function TJsonReader.StringEquals(const S: string): Boolean;
var
  N, I: Integer;
  Q: PChar;
begin
  if FEscaped then
    Exit(EscapedEquals(FP, FStart, FStop, S));
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
  M: QWord;
  D: Double;
begin
  if FToken <> jtNumber then
    Expected('a number');
  if FNumParsed then
    Exit;
  FNumParsed := True;
  M := FNumMant;
  if FNumIsInt and not FNumBig then
  begin
    { an integer: Int64, or UInt64 }
    if FNumNeg then
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
  if FNumIsInt and FNumBig then
  begin
    { 20 digits: a UInt64 if it fits }
    if not FNumNeg and (FStop - FStart = 20) and
       ((M < QWord(1844674407370955161)) or
        ((M = QWord(1844674407370955161)) and (FP[FStart + 19] <= '5'))) then
    begin
      FNumKind := jnUInt64;
      FUInt := M * 10 + QWord(Ord(FP[FStart + 19]) - Ord('0'));
      FDouble := FUInt;
      Exit;
    end;
  end;
  FNumKind := jnDouble;
  if not FNumBig then
  begin
    { exact: a small power of ten, or the Eisel-Lemire algorithm }
    if (M <= QWord(1) shl 53) and (FNumExp >= -22) and (FNumExp <= 22) then
    begin
      D := M;
      if FNumExp >= 0 then
        FDouble := D * Pow10[FNumExp]
      else
        FDouble := D / Pow10[-FNumExp];
    end
    else if M = 0 then
      FDouble := 0.0
    else
      FDouble := EiselLemire(M, FNumExp);
    if FNumNeg then
      FDouble := -FDouble;
  end
  else
    { more digits: through strtod, in place (the number was checked, and
      what follows it stops strtod) }
    FDouble := c_strtod(FP + FStart, nil);
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
    Expected('a boolean');
  end;
end;

{ --- writing --- }

function c_snprintf(buf: PChar; n: SizeInt; fmt: PChar): Integer; cdecl;
  varargs; external 'c' name 'snprintf';

{ P significant digits of |D|, correctly rounded, by C's printf (%.*e): the
  digits and the decimal exponent of the first }
procedure PrintfDigits(D: Double; P: Integer; var Dig: array of Integer;
                       out E: Integer);
var
  Buf: array[0..63] of Char;
  I, N, Sign: Integer;
begin
  c_snprintf(@Buf[0], 64, '%.*e', P - 1, D);
  I := 0;
  if Buf[0] = '-' then
    I := 1;
  N := 0;
  while Buf[I] <> 'e' do
  begin
    if Buf[I] <> '.' then
    begin
      Dig[N] := Ord(Buf[I]) - Ord('0');
      Inc(N);
    end;
    Inc(I);
  end;
  Inc(I);
  Sign := 1;
  if Buf[I] = '-' then
    Sign := -1;
  Inc(I);
  E := 0;
  while Buf[I] <> #0 do
  begin
    E := E * 10 + Ord(Buf[I]) - Ord('0');
    Inc(I);
  end;
  E := E * Sign;
end;

{ D as the shortest of "%.15g", "%.16g", "%.17g" that reads back as D (the
  text printf makes, plus ".0" for a number with no point or exponent).
  One printf gives 17 digits; the 16 and 15 digit candidates are those
  digits rounded (printf again only when the dropped digits are a tie that
  the 17 digit rounding hides), and a candidate is tested by reading it
  with Eisel-Lemire instead of strtod. }
function JsonFloat(D: Double): string;
var
  Dig, Cand: array[0..16] of Integer;
  Out: array[0..40] of Char;
  P, I, E, CE, N, T, X, O: Integer;
  M: QWord;
  Neg, Amb, Same: Boolean;
  V, A: Double;
begin
  if D - D <> 0 then
    raise EJsonError.Create('a number that is not finite cannot be written as JSON');
  Neg := PQWord(@D)^ shr 63 <> 0;
  if D = 0 then
  begin
    if Neg then
      Exit('-0.0');
    Exit('0.0');
  end;
  A := D;
  if Neg then
    A := -D;
  PrintfDigits(D, 17, Dig, E);
  P := 15;
  while True do
  begin
    CE := E;
    if P = 17 then
    begin
      for I := 0 to 16 do
        Cand[I] := Dig[I];
      Break;
    end;
    { round the 17 digits at P }
    Amb := False;
    if P = 15 then
    begin
      T := Dig[15] * 10 + Dig[16];
      Amb := T = 50;
    end
    else
    begin
      T := Dig[16];
      Amb := T = 5;
    end;
    if Amb then
      PrintfDigits(D, P, Cand, CE)
    else
    begin
      for I := 0 to P - 1 do
        Cand[I] := Dig[I];
      if ((P = 15) and (T > 50)) or ((P = 16) and (T > 5)) then
      begin
        I := P - 1;
        while (I >= 0) and (Cand[I] = 9) do
        begin
          Cand[I] := 0;
          Dec(I);
        end;
        if I >= 0 then
          Inc(Cand[I])
        else
        begin
          Cand[0] := 1;
          Inc(CE);
        end;
      end;
    end;
    M := 0;
    for I := 0 to P - 1 do
      M := M * 10 + QWord(Cand[I]);
    V := EiselLemire(M, CE - (P - 1));
    if V = A then
      Break;
    Inc(P);
  end;
  { %g: the digits without trailing zeros, in the fixed or the exponent form }
  N := P;
  while (N > 1) and (Cand[N - 1] = 0) do
    Dec(N);
  X := CE;
  O := 0;
  if Neg then
  begin
    Out[O] := '-';
    Inc(O);
  end;
  Same := False;       { (a point or an exponent was written) }
  if (X < -4) or (X >= P) then
  begin
    Out[O] := Chr(Ord('0') + Cand[0]);
    Inc(O);
    if N > 1 then
    begin
      Out[O] := '.';
      Inc(O);
      for I := 1 to N - 1 do
      begin
        Out[O] := Chr(Ord('0') + Cand[I]);
        Inc(O);
      end;
    end;
    Out[O] := 'e';
    Inc(O);
    if X < 0 then
    begin
      Out[O] := '-';
      X := -X;
    end
    else
      Out[O] := '+';
    Inc(O);
    if X >= 100 then
    begin
      Out[O] := Chr(Ord('0') + X div 100);
      Inc(O);
    end;
    Out[O] := Chr(Ord('0') + (X div 10) mod 10);
    Inc(O);
    Out[O] := Chr(Ord('0') + X mod 10);
    Inc(O);
    Same := True;
  end
  else if X >= 0 then
  begin
    for I := 0 to X do
    begin
      if I < N then
        Out[O] := Chr(Ord('0') + Cand[I])
      else
        Out[O] := '0';
      Inc(O);
    end;
    if N > X + 1 then
    begin
      Out[O] := '.';
      Inc(O);
      for I := X + 1 to N - 1 do
      begin
        Out[O] := Chr(Ord('0') + Cand[I]);
        Inc(O);
      end;
      Same := True;
    end;
  end
  else
  begin
    Out[O] := '0';
    Inc(O);
    Out[O] := '.';
    Inc(O);
    for I := 1 to -X - 1 do
    begin
      Out[O] := '0';
      Inc(O);
    end;
    for I := 0 to N - 1 do
    begin
      Out[O] := Chr(Ord('0') + Cand[I]);
      Inc(O);
    end;
    Same := True;
  end;
  if not Same then
  begin
    Out[O] := '.';
    Inc(O);
    Out[O] := '0';
    Inc(O);
  end;
  SetLength(Result, O);
  Move(Out[0], Result[1], O);
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

{ the digits of Q, written backwards into a stack buffer (no string made) }
procedure AddDigits(var Buf: string; var Len: Integer; Q: QWord; Negative: Boolean);
var
  Tmp: array[0..24] of AnsiChar;
  I: Integer;
begin
  I := 25;
  repeat
    Dec(I);
    Tmp[I] := AnsiChar(Ord('0') + Integer(Q mod 10));
    Q := Q div 10;
  until Q = 0;
  if Negative then
  begin
    Dec(I);
    Tmp[I] := '-';
  end;
  AddChars(Buf, Len, PChar(@Tmp[I]), 25 - I);
end;

procedure TJsonWriter.Value(I: Int64);
begin
  BeforeValue;
  if I < 0 then
    AddDigits(FBuf, FLen, QWord(-(I + 1)) + 1, True)    { (also Low(Int64)) }
  else
    AddDigits(FBuf, FLen, QWord(I), False);
end;

procedure TJsonWriter.ValueUInt64(Q: QWord);
begin
  BeforeValue;
  AddDigits(FBuf, FLen, Q, False);
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

function HashBytes(P: PChar; N: Integer): Cardinal;
var
  I: Integer;
  H: Cardinal;
begin
  H := 2166136261;
  for I := 0 to N - 1 do
    H := (H xor Cardinal(Ord(P[I]))) * 16777619;
  Result := H xor (H shr 15);
end;

function HashString(const S: string): Cardinal;
begin
  Result := HashBytes(PChar(S), Length(S));
end;

{ the hash of a key node's text (decoded) }
function KeyNodeHash(D: TJsonDocument; K: PJsonNode): Cardinal;
begin
  if K^.InSource and not K^.Escaped then
    Result := HashBytes(PChar(D.FSource) + K^.Start, K^.Len)
  else
    Result := HashString(D.NodeString(K));
end;

{ member M into the table (not when an earlier member has its key: the first
  one is found) }
procedure IndexAddMember(D: TJsonDocument; N: PJsonNode; M: Integer);
var
  Mask, Slot, J: Integer;
  Name: string;
  Named: Boolean;
begin
  Mask := Length(N^.Slots) - 1;
  Slot := Integer(KeyNodeHash(D, N^.Keys[M]) and Cardinal(Mask));
  Named := False;
  while N^.Slots[Slot] <> 0 do
  begin
    J := N^.Slots[Slot] - 1;
    if not Named then
    begin
      Name := D.NodeString(N^.Keys[M]);
      Named := True;
    end;
    if (J < M) and KeyIs(D, N^.Keys[J], Name) then
      Exit;
    Slot := (Slot + 1) and Mask;
  end;
  N^.Slots[Slot] := M + 1;
end;

procedure IndexBuild(D: TJsonDocument; N: PJsonNode);
var
  Size, M: Integer;
begin
  Size := 32;
  while Size < 2 * N^.Count do
    Size := Size * 2;
  N^.Slots := nil;
  SetLength(N^.Slots, Size);
  for M := 0 to N^.Count - 1 do
    IndexAddMember(D, N, M);
  N^.Indexed := N^.Count;
end;

function IndexOfKey(D: TJsonDocument; N: PJsonNode; const Key: string): Integer;
var
  I, Mask, Slot: Integer;
begin
  if N^.Count < 16 then
  begin
    for I := 0 to N^.Count - 1 do
      if KeyIs(D, N^.Keys[I], Key) then
        Exit(I);
    Exit(-1);
  end;
  if (Length(N^.Slots) = 0) or (N^.Indexed <> N^.Count) then
    IndexBuild(D, N);
  Mask := Length(N^.Slots) - 1;
  Slot := Integer(HashString(Key) and Cardinal(Mask));
  while N^.Slots[Slot] <> 0 do
  begin
    I := N^.Slots[Slot] - 1;
    if KeyIs(D, N^.Keys[I], Key) then
      Exit(I);
    Slot := (Slot + 1) and Mask;
  end;
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
  { keep a table up to date (a full one is made again when next needed) }
  if Length(FNode^.Slots) > 0 then
    if (FNode^.Indexed = FNode^.Count - 1) and (2 * FNode^.Count <= Length(FNode^.Slots)) then
    begin
      IndexAddMember(FDoc, FNode, FNode^.Count - 1);
      FNode^.Indexed := FNode^.Count;
    end
    else
      FNode^.Slots := nil;
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
  FNode^.Slots := nil;
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
  begin
    FNode^.Count := 0;
    FNode^.Slots := nil;
  end;
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

{ --- lazy access --- }

{ the extent of the value whose first token R just read: [Start, Stop) }
procedure ValueExtent(R: TJsonReader; out Start, Stop: Integer);
begin
  case R.Token of
    jtBeginObject, jtBeginArray:
      begin
        Start := R.TokenStart;
        R.Skip;
        Stop := R.Position;
      end;
    jtString:
      begin
        Start := R.TokenStart - 1;
        Stop := R.TokenStop + 1;
      end;
  else
    begin
      Start := R.TokenStart;
      Stop := R.TokenStop;
    end;
  end;
end;

function LazyOf(const Text: string; Start, Stop: Integer): TJsonLazy;
begin
  Result.FText := Text;
  Result.FStart := Start;
  Result.FLen := Stop - Start;
end;

function Missing(const Text: string): TJsonLazy;
begin
  Result.FText := Text;
  Result.FStart := 0;
  Result.FLen := -1;
end;

type
  { a step of a path: a name, an index, or (a JSON Pointer's) either }
  TStep = record
    Name: string;
    Index: Integer;   { -1: none }
    IsName: Boolean;
  end;
  TStepArray = array of TStep;

function KeySteps(const Path: array of TJsonKey): TStepArray;
var
  I: Integer;
begin
  SetLength(Result, Length(Path));
  for I := 0 to High(Path) do
  begin
    Result[I].Name := Path[I].Name;
    Result[I].Index := Path[I].Index;
    Result[I].IsName := Path[I].Index < 0;
  end;
end;

function PointerSteps(const Pointer: string): TStepArray;
var
  I, J, K, N, Idx: Integer;
  Part: string;
  Ok: Boolean;
begin
  SetLength(Result, 0);
  if Pointer = '' then
    Exit;
  if Pointer[1] <> '/' then
    raise EJsonError.Create('a JSON Pointer starts with ''/'': ''' + Pointer + '''');
  I := 2;
  N := 0;
  while True do
  begin
    J := I;
    while (J <= Length(Pointer)) and (Pointer[J] <> '/') do
      Inc(J);
    Part := StringReplace(StringReplace(Copy(Pointer, I, J - I), '~1', '/',
                          [rfReplaceAll]), '~0', '~', [rfReplaceAll]);
    SetLength(Result, N + 1);
    Result[N].Name := Part;
    Result[N].IsName := True;
    Ok := (Part <> '') and ((Part = '0') or (Part[1] <> '0'));
    Idx := 0;
    for K := 1 to Length(Part) do
      if Ok and (Part[K] in ['0'..'9']) and (Idx < 100000000) then
        Idx := Idx * 10 + Ord(Part[K]) - Ord('0')
      else
        Ok := False;
    if Ok then
      Result[N].Index := Idx
    else
      Result[N].Index := -1;
    Inc(N);
    if J > Length(Pointer) then
      Break;
    I := J + 1;
  end;
end;

function Walk(const Text: string; Start, Len: Integer;
              const Steps: TStepArray): TJsonLazy;
var
  R: TJsonReader;
  S, N, A, B: Integer;
  Found: Boolean;
begin
  Result := Missing(Text);
  if Len < 0 then
    Exit;
  R := TJsonReader.CreateRange(Text, Start, Len);
  try
    R.Read;
    for S := 0 to High(Steps) do
    begin
      Found := False;
      if (R.Token = jtBeginObject) and Steps[S].IsName then
      begin
        while R.Read and (R.Token = jtKey) do
        begin
          if R.StringEquals(Steps[S].Name) then
          begin
            R.Read; { its value }
            Found := True;
            Break;
          end;
          R.Skip;
        end;
      end
      else if (R.Token = jtBeginArray) and (Steps[S].Index >= 0) then
      begin
        N := 0;
        while R.Read and (R.Token <> jtEndArray) do
        begin
          if N = Steps[S].Index then
          begin
            Found := True;
            Break;
          end;
          R.Skip;
          Inc(N);
        end;
      end;
      if not Found then
        Exit;
    end;
    ValueExtent(R, A, B);
    Result := LazyOf(Text, A, B);
  finally
    R.Free;
  end;
end;

function JsonLazy(const Text: string): TJsonLazy;
var
  I, J: Integer;
begin
  { (without the whitespace around it) }
  I := 1;
  while (I <= Length(Text)) and (Text[I] in [' ', #9, #10, #13]) do
    Inc(I);
  J := Length(Text);
  while (J >= I) and (Text[J] in [' ', #9, #10, #13]) do
    Dec(J);
  Result := LazyOf(Text, I - 1, J);
end;

function JsonGet(const Text: string; const Path: array of TJsonKey): TJsonLazy;
begin
  Result := Walk(Text, 0, Length(Text), KeySteps(Path));
end;

function JsonGetPath(const Text, Pointer: string): TJsonLazy;
begin
  Result := Walk(Text, 0, Length(Text), PointerSteps(Pointer));
end;

function TJsonLazy.Get(const Path: array of TJsonKey): TJsonLazy;
begin
  Result := Walk(FText, FStart, FLen, KeySteps(Path));
end;

function TJsonLazy.GetPath(const Pointer: string): TJsonLazy;
begin
  Result := Walk(FText, FStart, FLen, PointerSteps(Pointer));
end;

function TJsonLazy.Exists: Boolean;
begin
  Result := FLen >= 0;
end;

function TJsonLazy.Kind: TJsonKind;
begin
  if FLen <= 0 then
    raise EJsonError.Create('the value is not there');
  case FText[FStart + 1] of
    '{': Result := jkObject;
    '[': Result := jkArray;
    '"': Result := jkString;
    't': Result := jkTrue;
    'f': Result := jkFalse;
    'n': Result := jkNull;
  else
    Result := jkNumber;
  end;
end;

function TJsonLazy.Raw: string;
begin
  if FLen < 0 then
    Result := ''
  else
    Result := Copy(FText, FStart + 1, FLen);
end;

function TJsonLazy.IsNull: Boolean;
begin
  Result := (FLen > 0) and (FText[FStart + 1] = 'n');
end;

function TJsonLazy.Reader: TJsonReader;
begin
  if FLen < 0 then
    raise EJsonError.Create('the value is not there');
  Result := TJsonReader.CreateRange(FText, FStart, FLen);
  Result.Read;
end;

function TJsonLazy.AsString: string;
var
  R: TJsonReader;
begin
  R := Reader;
  try
    if R.Token <> jtString then
      raise EJsonError.Create('expected a string, found ' + KindNames[Kind]);
    Result := R.AsString;
  finally
    R.Free;
  end;
end;

function TJsonLazy.AsInt64: Int64;
var
  R: TJsonReader;
begin
  R := Reader;
  try
    if R.Token <> jtNumber then
      raise EJsonError.Create('expected a number, found ' + KindNames[Kind]);
    Result := R.AsInt64;
  finally
    R.Free;
  end;
end;

function TJsonLazy.AsUInt64: QWord;
var
  R: TJsonReader;
begin
  R := Reader;
  try
    if R.Token <> jtNumber then
      raise EJsonError.Create('expected a number, found ' + KindNames[Kind]);
    Result := R.AsUInt64;
  finally
    R.Free;
  end;
end;

function TJsonLazy.AsDouble: Double;
var
  R: TJsonReader;
begin
  R := Reader;
  try
    if R.Token <> jtNumber then
      raise EJsonError.Create('expected a number, found ' + KindNames[Kind]);
    Result := R.AsDouble;
  finally
    R.Free;
  end;
end;

function TJsonLazy.AsBoolean: Boolean;
var
  R: TJsonReader;
begin
  R := Reader;
  try
    if not (R.Token in [jtTrue, jtFalse]) then
      raise EJsonError.Create('expected a boolean, found ' + KindNames[Kind]);
    Result := R.Token = jtTrue;
  finally
    R.Free;
  end;
end;

function TJsonLazy.Parse: TJsonDocument;
begin
  if FLen < 0 then
    raise EJsonError.Create('the value is not there');
  Result := JsonParse(Raw);
end;

function JsonArrayItems(const L: TJsonLazy): iterator TJsonLazy;
var
  R: TJsonReader;
  A, B: Integer;
begin
  if L.FLen < 0 then
    Exit;
  R := TJsonReader.CreateRange(L.FText, L.FStart, L.FLen);
  try
    R.Read;
    if R.Token <> jtBeginArray then
      raise EJsonError.Create('expected an array, found ' + KindNames[L.Kind]);
    while R.Read and (R.Token <> jtEndArray) do
    begin
      ValueExtent(R, A, B);
      yield LazyOf(L.FText, A, B);
    end;
  finally
    R.Free;
  end;
end;

function JsonArrayItems(const Text: string): iterator TJsonLazy;
begin
  yield from JsonArrayItems(JsonLazy(Text));
end;

function JsonObjectItems(const L: TJsonLazy): iterator TJsonLazyMember;
var
  R: TJsonReader;
  A, B: Integer;
  M: TJsonLazyMember;
begin
  if L.FLen < 0 then
    Exit;
  R := TJsonReader.CreateRange(L.FText, L.FStart, L.FLen);
  try
    R.Read;
    if R.Token <> jtBeginObject then
      raise EJsonError.Create('expected an object, found ' + KindNames[L.Kind]);
    while R.Read and (R.Token = jtKey) do
    begin
      M.Key := R.AsString;
      R.Read;
      ValueExtent(R, A, B);
      M.Value := LazyOf(L.FText, A, B);
      yield M;
    end;
  finally
    R.Free;
  end;
end;

function JsonObjectItems(const Text: string): iterator TJsonLazyMember;
begin
  yield from JsonObjectItems(JsonLazy(Text));
end;

end.
