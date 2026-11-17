{ Untyped parameters (Turbo/FPC):  var x;  const x. Any variable is passed
  by reference; the routine uses it through typecasts (also assigned to,
  and with selectors), its address, move and fillchar, or passes it on to
  another untyped parameter. Also in methods, procedural types, external
  C routines (const x: a void pointer) and overloads (a typed parameter
  fits better). The output is identical to Free Pascal 3.2.2's
  (fpc -Mobjfpc).
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t > %t.out
  RUN: diff %S/untyped.expected %t.out
  RUN: %p3 --mode=objfpc -O2 %s -o %t.O2
  RUN: %t.O2 > %t.O2.out
  RUN: diff %S/untyped.expected %t.O2.out
}
program untyped;

type
  TPoint = record x, y: longint; end;
  TBytes4 = array[0..3] of byte;
  PLongint = ^longint;
  TStr = string;
  TSink = procedure(const data; size: longint);
  TBuffer = class
    Data: array[0..15] of byte;
    Used: longint;
    procedure Append(const src; n: longint);
    procedure Take(var dst; n: longint);
  end;

function memcmp(const a; const b; n: int64): longint; cdecl; external 'c';

procedure SetLong(var x; v: longint);
begin
  longint(x) := v;
end;

function GetLong(const x): longint;
begin
  GetLong := longint(x);
end;

procedure SwapAny(var a, b; size: longint);
var
  tmp: array[0..63] of byte;
begin
  move(a, tmp, size);
  move(b, a, size);
  move(tmp, b, size);
end;

procedure Clear(var x; size: longint);
begin
  fillchar(x, size, 0);
end;

procedure MovePoint(var p; dx, dy: longint);
begin
  TPoint(p).x := TPoint(p).x + dx;
  TPoint(p).y := TPoint(p).y + dy;
end;

function ByteAt(const x; i: longint): byte;
begin
  ByteAt := TBytes4(x)[i];
end;

function Describe(const s): string;
begin
  Describe := '<' + TStr(s) + '>';
end;

procedure Forward(var x);
begin
  SetLong(x, GetLong(x) * 10); { passed on to untyped parameters }
end;

function AddressOf(const x): pointer;
begin
  AddressOf := @x;
end;

procedure Dump(const data; size: longint);
var
  i: longint;
begin
  for i := 0 to size - 1 do
    write(TBytes4(data)[i], ' ');
  writeln;
end;

procedure Show(var x: longint); overload;
begin
  writeln('typed ', x);
end;

procedure Show(var x); overload;
begin
  writeln('untyped ', GetLong(x));
end;

procedure TBuffer.Append(const src; n: longint);
begin
  move(src, Data[Used], n);
  Used := Used + n;
end;

procedure TBuffer.Take(var dst; n: longint);
begin
  move(Data[0], dst, n);
end;

var
  i, j: longint;
  w: word;
  p, q: TPoint;
  s: string;
  sink: TSink;
  buf: TBuffer;

begin
  SetLong(i, 42);
  writeln(i, ' ', GetLong(i));
  j := 7;
  SwapAny(i, j, sizeof(longint));
  writeln(i, ' ', j);

  p.x := 1; p.y := 2;
  q.x := 30; q.y := 40;
  SwapAny(p, q, sizeof(TPoint));
  MovePoint(p, 5, 6);
  writeln(p.x, ' ', p.y, ' ', q.x, ' ', q.y);
  Clear(q, sizeof(q));
  writeln(q.x, ' ', q.y);

  i := $04030201;
  writeln(ByteAt(i, 0), ' ', ByteAt(i, 3));
  s := 'hello';
  writeln(Describe(s));
  Forward(j);
  writeln(j);
  writeln(AddressOf(i) = @i, ' ', PLongint(AddressOf(i))^ = i);

  w := 513;
  Show(i);
  Show(p); { only the untyped one fits a record: its first longint }

  sink := @Dump;
  sink(i, 4);
  i := 5; j := 5;
  writeln(memcmp(i, j, sizeof(i)) = 0);
  j := 6;
  writeln(memcmp(i, j, sizeof(i)) < 0);

  buf := TBuffer.Create;
  i := $11223344;
  buf.Append(i, 4);
  buf.Append(w, 2);
  writeln(buf.Used, ' ', buf.Data[0], ' ', buf.Data[4]);
  j := 0;
  buf.Take(j, 4);
  writeln(j = i);
  buf.Free;
end.
