{ SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception }
unit pcutil;
{ Support library of the self-hosted compiler:

    arena      zeroed memory that lives until the compiler exits
    names      interned strings (identifiers, string constants): equal
               strings give the same PName, compared by pointer
    lists      growable lists of pointers
    maps       hash maps from int64 keys (e.g. name ids) to pointers
    buffers    growable text buffers for output of any length

  Nothing iterates over a hash table, so output never depends on memory
  addresses (SELFHOST.md RT2). }

interface

uses pcsys;

type
  TPtrArray = array[0..$FFFFFFF] of pointer;
  PPtrArray = ^TPtrArray;

  TPtrList = record
    items: PPtrArray;
    count, cap: longint;
  end;

  PName = ^TName;
  TName = record
    id: longint;       { 1, 2, ... in the order names were first interned }
    hash: cardinal;
    len: longint;
    chars: PCharArray; { len characters followed by #0 }
    next: PName;       { next name in the same pool bucket }
  end;

  PMapEntry = ^TMapEntry;
  TMapEntry = record
    key: int64;
    value: pointer;
    next: PMapEntry;
  end;

  TIntMap = record
    buckets: PPtrArray; { of PMapEntry; nil until the first insertion }
    size, count: longint;
  end;

  TBuf = record
    data: PCharArray;
    len, cap: int64;
  end;

{ Arena }
function arenaAlloc(size: int64): pointer;

{ Lists }
procedure listInit(var l: TPtrList);
procedure listAdd(var l: TPtrList; p: pointer);
function listGet(const l: TPtrList; i: longint): pointer;
procedure listSet(var l: TPtrList; i: longint; p: pointer);
function listPop(var l: TPtrList): pointer;
procedure listClear(var l: TPtrList);
procedure listFree(var l: TPtrList);

{ Names }
function hashChars(p: PCharArray; start, count: int64): cardinal;
function internChars(p: PCharArray; start, count: int64): PName;
function intern(const s: string): PName;
function nameStr(n: PName): string;
function nameEquals(n: PName; const s: string): boolean;
function nameCount: longint;

{ Maps }
procedure mapInit(var m: TIntMap);
function mapGet(const m: TIntMap; key: int64): pointer;
function mapHas(const m: TIntMap; key: int64): boolean;
procedure mapPut(var m: TIntMap; key: int64; value: pointer);

{ Buffers }
procedure bufInit(var b: TBuf);
procedure bufClear(var b: TBuf);
procedure bufFree(var b: TBuf);
procedure bufAddChar(var b: TBuf; c: char);
procedure bufAdd(var b: TBuf; const s: string);
procedure bufAddChars(var b: TBuf; p: PCharArray; start, count: int64);
procedure bufAddName(var b: TBuf; n: PName);
procedure bufAddInt(var b: TBuf; v: int64);
function bufCharAt(const b: TBuf; i: int64): char;
procedure bufWrite(const b: TBuf; f: pointer);

{ Text }
function intToStr(v: int64): string;
{ v's 64 bits read as an unsigned number (literals up to 2^64-1). }
function uintToStr(v: int64): string;
{ Unsigned 64-bit arithmetic on the bits of int64s (qword values):
  uCompare is -1, 0 or 1; uDiv and uMod require b <> 0; uToReal rounds as
  a C uint64_t to double conversion. }
function uCompare(a, b: int64): longint;
function uDiv(a, b: int64): int64;
function uMod(a, b: int64): int64;
function uToReal(a: int64): double;
{ C printf "%.<prec>g" of x, exactly (correctly rounded, as glibc). }
function formatG(x: double; prec: longint): string;
{ Appends p[start..start+count) escaped like llvm::raw_ostream::
  write_escaped: \\ \t \n \" and octal escapes for unprintable bytes. }
procedure bufAddEscaped(var b: TBuf; p: PCharArray; start, count: int64);
function lowerChar(c: char): char;
function lowerStr(const s: string): string;
{ Real arithmetic that never traps. Free Pascal stops on floating-point
  overflow; these return +-infinity instead, exactly like IEEE arithmetic
  (the C++ compiler's). realDiv requires y <> 0. }
function realInfinity(negative: boolean): double;
function realAdd(x, y: double): double;
function realSub(x, y: double): double;
function realMul(x, y: double): double;
function realDiv(x, y: double): double;
function realIsInf(x: double): boolean;
function realIsNaN(x: double): boolean;
{ A quiet NaN (as ARM produces for inf - inf; x86's default NaN also has
  the sign bit set). }
function realNaN: double;

{ Paths (llvm::sys::path): the directory part ('' for a bare name), and a
  directory joined with a name. }
function pathDirName(const path: string): string;
function pathJoin(const dir, name: string): string;

implementation

const
  chunkSize = 1048576;

var
  chunk: PCharArray; { current arena chunk }
  chunkUsed: int64;
  poolBuckets: PPtrArray;
  poolSize, poolCount: longint;

{ --- Arena --- }

function arenaAlloc(size: int64): pointer;
var
  p: pointer;
begin
  size := (size + 7) div 8 * 8;
  if size = 0 then
    size := 8;
  if size > chunkSize div 4 then
  begin
    { large blocks get their own allocation }
    getmem(p, size);
    fillchar(PCharArray(p)^, size, 0);
    exit(p);
  end;
  if (chunk = nil) or (chunkUsed + size > chunkSize) then
  begin
    getmem(chunk, chunkSize);
    fillchar(chunk^, chunkSize, 0);
    chunkUsed := 0;
  end;
  {$R-}
  p := @chunk^[chunkUsed];
  {$R+}
  chunkUsed := chunkUsed + size;
  arenaAlloc := p;
end;

{ --- Lists --- }

procedure listInit(var l: TPtrList);
begin
  l.items := nil;
  l.count := 0;
  l.cap := 0;
end;

procedure listAdd(var l: TPtrList; p: pointer);
begin
  if l.count = l.cap then
  begin
    if l.cap = 0 then
      l.cap := 8
    else
      l.cap := l.cap * 2;
    reallocmem(l.items, int64(l.cap) * sizeof(pointer));
  end;
  {$R-}
  l.items^[l.count] := p;
  {$R+}
  l.count := l.count + 1;
end;

function listGet(const l: TPtrList; i: longint): pointer;
begin
  if (i < 0) or (i >= l.count) then
  begin
    sysWrite(sysErr, 'internal error: list index out of range' + #10);
    sysExit(3);
  end;
  {$R-}
  listGet := l.items^[i];
  {$R+}
end;

procedure listSet(var l: TPtrList; i: longint; p: pointer);
begin
  if (i < 0) or (i >= l.count) then
  begin
    sysWrite(sysErr, 'internal error: list index out of range' + #10);
    sysExit(3);
  end;
  {$R-}
  l.items^[i] := p;
  {$R+}
end;

function listPop(var l: TPtrList): pointer;
begin
  listPop := listGet(l, l.count - 1);
  l.count := l.count - 1;
end;

procedure listClear(var l: TPtrList);
begin
  l.count := 0;
end;

procedure listFree(var l: TPtrList);
begin
  freemem(l.items);
  listInit(l);
end;

{ --- Names --- }

{ FNV-1a }
function hashChars(p: PCharArray; start, count: int64): cardinal;
var
  h: cardinal;
  i: int64;
begin
  {$Q-}{$R-}
  h := 2166136261;
  for i := start to start + count - 1 do
  begin
    h := h xor ord(p^[i]);
    h := h * 16777619;
  end;
  {$Q+}{$R+}
  hashChars := h;
end;

function sameChars(n: PName; p: PCharArray; start, count: int64): boolean;
var
  i: int64;
begin
  if n^.len <> count then
    exit(false);
  {$R-}
  for i := 0 to count - 1 do
    if n^.chars^[i] <> p^[start + i] then
      exit(false);
  {$R+}
  sameChars := true;
end;

function poolBucket(h: cardinal): longint;
begin
  poolBucket := longint(h and cardinal(poolSize - 1));
end;

procedure growPool;
var
  old: PPtrArray;
  oldSize, i, b: longint;
  n, next: PName;
begin
  old := poolBuckets;
  oldSize := poolSize;
  if poolSize = 0 then
    poolSize := 1024
  else
    poolSize := poolSize * 2;
  getmem(poolBuckets, int64(poolSize) * sizeof(pointer));
  fillchar(poolBuckets^, int64(poolSize) * sizeof(pointer), 0);
  {$R-}
  for i := 0 to oldSize - 1 do
  begin
    n := PName(old^[i]);
    while n <> nil do
    begin
      next := n^.next;
      b := poolBucket(n^.hash);
      n^.next := PName(poolBuckets^[b]);
      poolBuckets^[b] := n;
      n := next;
    end;
  end;
  {$R+}
  freemem(old);
end;

function internChars(p: PCharArray; start, count: int64): PName;
var
  h: cardinal;
  b: longint;
  n: PName;
begin
  if poolCount >= poolSize * 3 div 4 then
    growPool;
  h := hashChars(p, start, count);
  b := poolBucket(h);
  {$R-}
  n := PName(poolBuckets^[b]);
  {$R+}
  while n <> nil do
  begin
    if (n^.hash = h) and sameChars(n, p, start, count) then
      exit(n);
    n := n^.next;
  end;
  n := PName(arenaAlloc(sizeof(TName)));
  poolCount := poolCount + 1;
  n^.id := poolCount;
  n^.hash := h;
  n^.len := longint(count);
  n^.chars := PCharArray(arenaAlloc(count + 1)); { zeroed: ends with #0 }
  if count > 0 then
    {$R-}
    move(p^[start], n^.chars^[0], count);
    {$R+}
  {$R-}
  n^.next := PName(poolBuckets^[b]);
  poolBuckets^[b] := n;
  {$R+}
  internChars := n;
end;

function intern(const s: string): PName;
begin
  if length(s) = 0 then
    intern := internChars(nil, 0, 0)
  else
    intern := internChars(PCharArray(@s[1]), 0, length(s));
end;

function nameStr(n: PName): string;
var
  s: string;
  i, len: longint;
begin
  s := '';
  len := n^.len;
  if len > 255 then
    len := 255;
  {$R-}
  for i := 0 to len - 1 do
    s := s + n^.chars^[i];
  {$R+}
  nameStr := s;
end;

function nameEquals(n: PName; const s: string): boolean;
begin
  if length(s) = 0 then
    nameEquals := n^.len = 0
  else
    nameEquals := sameChars(n, PCharArray(@s[1]), 0, length(s));
end;

function nameCount: longint;
begin
  nameCount := poolCount;
end;

{ --- Maps --- }

procedure mapInit(var m: TIntMap);
begin
  m.buckets := nil;
  m.size := 0;
  m.count := 0;
end;

function mapBucket(const m: TIntMap; key: int64): longint;
var
  h: cardinal;
begin
  {$Q-}{$R-}
  h := cardinal(key) xor cardinal(key shr 32);
  h := h * 2654435761;
  {$Q+}{$R+}
  mapBucket := longint(h and cardinal(m.size - 1));
end;

function findEntry(const m: TIntMap; key: int64): PMapEntry;
var
  e: PMapEntry;
begin
  if m.size = 0 then
    exit(nil);
  {$R-}
  e := PMapEntry(m.buckets^[mapBucket(m, key)]);
  {$R+}
  while (e <> nil) and (e^.key <> key) do
    e := e^.next;
  findEntry := e;
end;

function mapGet(const m: TIntMap; key: int64): pointer;
var
  e: PMapEntry;
begin
  e := findEntry(m, key);
  if e = nil then
    mapGet := nil
  else
    mapGet := e^.value;
end;

function mapHas(const m: TIntMap; key: int64): boolean;
begin
  mapHas := findEntry(m, key) <> nil;
end;

procedure growMap(var m: TIntMap);
var
  old: PPtrArray;
  oldSize, i, b: longint;
  e, next: PMapEntry;
begin
  old := m.buckets;
  oldSize := m.size;
  if m.size = 0 then
    m.size := 16
  else
    m.size := m.size * 2;
  getmem(m.buckets, int64(m.size) * sizeof(pointer));
  fillchar(m.buckets^, int64(m.size) * sizeof(pointer), 0);
  {$R-}
  for i := 0 to oldSize - 1 do
  begin
    e := PMapEntry(old^[i]);
    while e <> nil do
    begin
      next := e^.next;
      b := mapBucket(m, e^.key);
      e^.next := PMapEntry(m.buckets^[b]);
      m.buckets^[b] := e;
      e := next;
    end;
  end;
  {$R+}
  freemem(old);
end;

procedure mapPut(var m: TIntMap; key: int64; value: pointer);
var
  e: PMapEntry;
  b: longint;
begin
  e := findEntry(m, key);
  if e <> nil then
  begin
    e^.value := value;
    exit;
  end;
  if m.count >= m.size * 3 div 4 then
    growMap(m);
  e := PMapEntry(arenaAlloc(sizeof(TMapEntry)));
  e^.key := key;
  e^.value := value;
  b := mapBucket(m, key);
  {$R-}
  e^.next := PMapEntry(m.buckets^[b]);
  m.buckets^[b] := e;
  {$R+}
  m.count := m.count + 1;
end;

{ --- Buffers --- }

procedure bufInit(var b: TBuf);
begin
  b.data := nil;
  b.len := 0;
  b.cap := 0;
end;

procedure bufClear(var b: TBuf);
begin
  b.len := 0;
end;

procedure bufFree(var b: TBuf);
begin
  freemem(b.data);
  bufInit(b);
end;

procedure bufReserve(var b: TBuf; extra: int64);
begin
  if b.len + extra <= b.cap then
    exit;
  if b.cap = 0 then
    b.cap := 256;
  while b.len + extra > b.cap do
    b.cap := b.cap * 2;
  reallocmem(b.data, b.cap);
end;

procedure bufAddChar(var b: TBuf; c: char);
begin
  bufReserve(b, 1);
  {$R-}
  b.data^[b.len] := c;
  {$R+}
  b.len := b.len + 1;
end;

procedure bufAdd(var b: TBuf; const s: string);
begin
  if length(s) > 0 then
    bufAddChars(b, PCharArray(@s[1]), 0, length(s));
end;

procedure bufAddChars(var b: TBuf; p: PCharArray; start, count: int64);
begin
  if count <= 0 then
    exit;
  bufReserve(b, count);
  {$R-}
  move(p^[start], b.data^[b.len], count);
  {$R+}
  b.len := b.len + count;
end;

procedure bufAddName(var b: TBuf; n: PName);
begin
  bufAddChars(b, n^.chars, 0, n^.len);
end;

procedure bufAddInt(var b: TBuf; v: int64);
begin
  bufAdd(b, intToStr(v));
end;

function bufCharAt(const b: TBuf; i: int64): char;
begin
  {$R-}
  bufCharAt := b.data^[i];
  {$R+}
end;

procedure bufWrite(const b: TBuf; f: pointer);
begin
  sysWriteChars(f, b.data, 0, b.len);
end;

{ --- Text --- }

function intToStr(v: int64): string;
var
  s: string;
  neg: boolean;
  digit: longint;
begin
  if v = 0 then
    exit('0');
  neg := v < 0;
  s := '';
  { work with non-positive values: low(int64) has no positive counterpart }
  if not neg then
    v := -v;
  while v <> 0 do
  begin
    digit := longint(-(v mod 10));
    s := chr(ord('0') + digit) + s;
    v := v div 10;
  end;
  if neg then
    s := '-' + s;
  intToStr := s;
end;

function uintToStr(v: int64): string;
var
  q, r: int64;
begin
  if v >= 0 then
    exit(intToStr(v));
  { u div 10 = (u shr 1) div 5; the remainder wraps back into 0..9 }
  q := (v shr 1) div 5;
  {$Q-}{$R-}
  r := v - q * 10;
  {$Q+}{$R+}
  uintToStr := intToStr(q) + chr(ord('0') + longint(r));
end;

function uCompare(a, b: int64): longint;
var
  x, y: int64;
begin
  { flipping the sign bit maps unsigned order onto signed order }
  {$Q-}{$R-}
  x := a xor (-9223372036854775807 - 1);
  y := b xor (-9223372036854775807 - 1);
  {$Q+}{$R+}
  if x < y then
    uCompare := -1
  else if x > y then
    uCompare := 1
  else
    uCompare := 0;
end;

function uDiv(a, b: int64): int64;
var
  q, r: int64;
begin
  if b < 0 then { b >= 2^63: the quotient is 0 or 1 }
  begin
    if uCompare(a, b) >= 0 then
      exit(1);
    exit(0);
  end;
  if a >= 0 then
    exit(a div b);
  { a >= 2^63: halve it (exactly, as a natural number), divide, then
    correct the quotient by at most one }
  {$Q-}{$R-}
  q := ((a shr 1) div b) shl 1;
  r := a - q * b;
  {$Q+}{$R+}
  if uCompare(r, b) >= 0 then
    q := q + 1;
  uDiv := q;
end;

function uMod(a, b: int64): int64;
var
  q: int64;
begin
  q := uDiv(a, b);
  {$Q-}{$R-}
  uMod := a - q * b;
  {$Q+}{$R+}
end;

function uToReal(a: int64): double;
var
  h: int64;
  x: double;
begin
  if a >= 0 then
  begin
    x := a;
    exit(x);
  end;
  { halve it, keeping the lowest bit as a sticky bit, so that the one
    rounding to 53 bits is the same as the whole number's }
  h := (a shr 1) or (a and 1);
  x := h;
  uToReal := x * 2;
end;

type
  TDoubleBits = record
    case boolean of
      true: (d: double);
      false: (bits: int64);
  end;
  { a big natural number: base 1e9 limbs, least significant first }
  TBigNum = record
    limb: array[0..127] of int64;
    count: longint;
  end;
  TDigits = array[0..1199] of char;

procedure bigMul(var n: TBigNum; factor: int64);
var
  i: longint;
  carry, t: int64;
begin
  carry := 0;
  for i := 0 to n.count - 1 do
  begin
    t := n.limb[i] * factor + carry;
    n.limb[i] := t mod 1000000000;
    carry := t div 1000000000;
  end;
  while carry > 0 do
  begin
    n.limb[n.count] := carry mod 1000000000;
    carry := carry div 1000000000;
    n.count := n.count + 1;
  end;
end;

{ The decimal digits of n into d (most significant first); returns the
  number of digits. }
function bigDigits(const n: TBigNum; var d: TDigits): longint;
var
  i, j, len: longint;
  v: int64;
  part: array[0..8] of char;
begin
  len := 0;
  for i := n.count - 1 downto 0 do
  begin
    v := n.limb[i];
    for j := 8 downto 0 do
    begin
      part[j] := chr(ord('0') + longint(v mod 10));
      v := v div 10;
    end;
    for j := 0 to 8 do
      if (len > 0) or (part[j] <> '0') or ((i = 0) and (j = 8)) then
      begin
        d[len] := part[j];
        len := len + 1;
      end;
  end;
  bigDigits := len;
end;

function formatG(x: double; prec: longint): string;
var
  o: TDoubleBits;
  neg: boolean;
  expBits, e2, len, decExp, i, k: longint;
  mant: int64;
  n: TBigNum;
  d: TDigits;
  roundUp: boolean;
  s, mantissa, frac: string;
begin
  o.d := x;
  neg := o.bits < 0;
  expBits := longint((o.bits shr 52) and $7FF);
  mant := o.bits and $FFFFFFFFFFFFF;
  if neg then
    s := '-'
  else
    s := '';
  if expBits = $7FF then
  begin
    if mant = 0 then
      exit(s + 'inf');
    exit(s + 'nan');
  end;
  if (expBits = 0) and (mant = 0) then
    exit(s + '0');
  if prec <= 0 then
    prec := 1;
  { x = mant * 2^e2 }
  if expBits = 0 then
    e2 := -1074
  else
  begin
    mant := mant + (int64(1) shl 52);
    e2 := expBits - 1075;
  end;
  { exact decimal digits: x = digits * 10^decExp }
  n.count := 0;
  while mant > 0 do
  begin
    n.limb[n.count] := mant mod 1000000000;
    mant := mant div 1000000000;
    n.count := n.count + 1;
  end;
  decExp := 0;
  if e2 >= 0 then
    for i := 1 to e2 do
      bigMul(n, 2)
  else
  begin
    { mant / 2^k = mant * 5^k / 10^k }
    for i := 1 to -e2 do
      bigMul(n, 5);
    decExp := e2;
  end;
  len := bigDigits(n, d);
  { round to prec significant digits, half to even on the exact value }
  if len > prec then
  begin
    roundUp := false;
    if d[prec] > '5' then
      roundUp := true
    else if d[prec] = '5' then
    begin
      for k := prec + 1 to len - 1 do
        if d[k] <> '0' then
          roundUp := true;
      if not roundUp then
        roundUp := odd(ord(d[prec - 1]) - ord('0'));
    end;
    decExp := decExp + (len - prec);
    len := prec;
    if roundUp then
    begin
      k := len - 1;
      while (k >= 0) and (d[k] = '9') do
      begin
        d[k] := '0';
        k := k - 1;
      end;
      if k >= 0 then
        d[k] := chr(ord(d[k]) + 1)
      else
      begin
        { 999 -> 1000: one more digit; drop the last zero }
        for k := len - 1 downto 1 do
          d[k] := d[k - 1];
        d[0] := '1';
        decExp := decExp + 1;
      end;
    end;
  end;
  while len < prec do
  begin
    d[len] := '0';
    len := len + 1;
    decExp := decExp - 1;
  end;
  { the exponent of the first digit }
  k := len - 1 + decExp;
  if (k < -4) or (k >= prec) then
  begin
    mantissa := d[0];
    frac := '';
    for i := 1 to prec - 1 do
      frac := frac + d[i];
    while (length(frac) > 0) and (frac[length(frac)] = '0') do
      delete(frac, length(frac), 1);
    if frac <> '' then
      mantissa := mantissa + '.' + frac;
    if k < 0 then
      mantissa := mantissa + 'e-'
    else
      mantissa := mantissa + 'e+';
    if abs(k) < 10 then
      mantissa := mantissa + '0';
    formatG := s + mantissa + intToStr(abs(k));
    exit;
  end;
  { fixed notation with prec - 1 - k fraction digits }
  if k >= 0 then
  begin
    for i := 0 to k do
      s := s + d[i];
    frac := '';
    for i := k + 1 to prec - 1 do
      frac := frac + d[i];
  end
  else
  begin
    s := s + '0';
    frac := '';
    for i := 1 to -k - 1 do
      frac := frac + '0';
    for i := 0 to prec - 1 do
      frac := frac + d[i];
  end;
  while (length(frac) > 0) and (frac[length(frac)] = '0') do
    delete(frac, length(frac), 1);
  if frac <> '' then
    s := s + '.' + frac;
  formatG := s;
end;

procedure bufAddEscaped(var b: TBuf; p: PCharArray; start, count: int64);
var
  i: int64;
  c: char;
begin
  {$R-}
  for i := start to start + count - 1 do
  begin
    c := p^[i];
    case c of
      '\': bufAdd(b, '\\');
      #9: bufAdd(b, '\t');
      #10: bufAdd(b, '\n');
      '"': bufAdd(b, '\"');
    else
      if (c >= ' ') and (c <= '~') then
        bufAddChar(b, c)
      else
      begin
        bufAddChar(b, '\');
        bufAddChar(b, chr(ord('0') + (ord(c) shr 6) and 7));
        bufAddChar(b, chr(ord('0') + (ord(c) shr 3) and 7));
        bufAddChar(b, chr(ord('0') + ord(c) and 7));
      end;
    end;
  end;
  {$R+}
end;

function pathDirName(const path: string): string;
var
  i: longint;
begin
  i := length(path);
  while (i > 0) and (path[i] <> '/') do
    i := i - 1;
  if i = 0 then
    pathDirName := ''
  else if i = 1 then
    pathDirName := '/'
  else
    pathDirName := copy(path, 1, i - 1);
end;

function pathJoin(const dir, name: string): string;
begin
  if dir = '' then
    pathJoin := name
  else if dir[length(dir)] = '/' then
    pathJoin := dir + name
  else
    pathJoin := dir + '/' + name;
end;

function realInfinity(negative: boolean): double;
var
  o: TDoubleBits;
begin
  o.bits := $7FF0000000000000;
  if negative then
    o.bits := o.bits or low(int64);
  realInfinity := o.d;
end;

function realIsNaN(x: double): boolean;
var
  o: TDoubleBits;
begin
  o.d := x;
  realIsNaN := ((o.bits and $7FF0000000000000) = $7FF0000000000000) and
               ((o.bits and $FFFFFFFFFFFFF) <> 0);
end;

function realNaN: double;
var
  o: TDoubleBits;
begin
  o.bits := $7FF8000000000000;
  realNaN := o.d;
end;

function realIsInf(x: double): boolean;
var
  o: TDoubleBits;
begin
  o.d := x;
  realIsInf := (o.bits and $7FFFFFFFFFFFFFFF) = $7FF0000000000000;
end;

{ The binary exponent e with 2^e <= |x| < 2^(e+1); x <> 0, finite. }
function realExponent(x: double): longint;
var
  o: TDoubleBits;
  e: longint;
  m: int64;
begin
  o.d := x;
  e := longint((o.bits shr 52) and $7FF);
  if e > 0 then
    exit(e - 1023);
  m := o.bits and $FFFFFFFFFFFFF; { subnormal }
  e := -1075;
  while m <> 0 do
  begin
    m := m shr 1;
    e := e + 1;
  end;
  realExponent := e;
end;

function maxDouble: double;
var
  o: TDoubleBits;
begin
  o.bits := $7FEFFFFFFFFFFFFF;
  maxDouble := o.d;
end;

function realAdd(x, y: double): double;
var
  h: double;
begin
  if realIsNaN(x) or realIsNaN(y) then
    exit(realNaN);
  if realIsInf(x) and realIsInf(y) and ((x < 0) <> (y < 0)) then
    exit(realNaN); { inf - inf }
  if (x = 0) or (y = 0) or realIsInf(x) or realIsInf(y) then
    exit(x + y);
  { below 2^1023 each, the sum cannot round past the largest double }
  if (realExponent(x) <= 1022) and (realExponent(y) <= 1022) then
    exit(x + y);
  { halving is exact at this size: fl(x/2 + y/2) = fl(x + y) / 2 }
  h := x * 0.5 + y * 0.5;
  if abs(h) > maxDouble * 0.5 then
    exit(realInfinity(h < 0));
  realAdd := h * 2;
end;

function realSub(x, y: double): double;
begin
  realSub := realAdd(x, -y);
end;

function realMul(x, y: double): double;
var
  e: longint;
  p: double;
begin
  if realIsNaN(x) or realIsNaN(y) then
    exit(realNaN);
  if ((x = 0) and realIsInf(y)) or ((y = 0) and realIsInf(x)) then
    exit(realNaN); { 0 * inf }
  if (x = 0) or (y = 0) or realIsInf(x) or realIsInf(y) then
    exit(x * y);
  e := realExponent(x) + realExponent(y);
  if e <= 1021 then { |xy| < 2^(e+2) <= 2^1023 }
    exit(x * y);
  if e >= 1024 then { |xy| >= 2^1024 }
    exit(realInfinity((x < 0) <> (y < 0)));
  p := (x * 0.25) * y; { exact scaling by 1/4 }
  if abs(p) > maxDouble * 0.25 then
    exit(realInfinity(p < 0));
  realMul := p * 4;
end;

function realDiv(x, y: double): double;
var
  e: longint;
  q: double;
begin
  if realIsNaN(x) or realIsNaN(y) or (realIsInf(x) and realIsInf(y)) then
    exit(realNaN); { inf / inf }
  if (x = 0) or realIsInf(x) or realIsInf(y) then
    exit(x / y);
  e := realExponent(x) - realExponent(y);
  if e <= 1021 then { |x/y| < 2^(e+1) <= 2^1022 }
    exit(x / y);
  if e >= 1025 then { |x/y| > 2^(e-1) >= 2^1024 }
    exit(realInfinity((x < 0) <> (y < 0)));
  q := (x * 0.125) / y; { exact scaling by 1/8 }
  if abs(q) > maxDouble * 0.125 then
    exit(realInfinity(q < 0));
  realDiv := q * 8;
end;

function lowerChar(c: char): char;
begin
  if (c >= 'A') and (c <= 'Z') then
    lowerChar := chr(ord(c) + 32)
  else
    lowerChar := c;
end;

function lowerStr(const s: string): string;
var
  r: string;
  i: longint;
begin
  r := '';
  for i := 1 to length(s) do
    r := r + lowerChar(s[i]);
  lowerStr := r;
end;

end.
