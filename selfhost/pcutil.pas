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
function lowerChar(c: char): char;
function lowerStr(const s: string): string;

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
