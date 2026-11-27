program wunsafe_tp;

uses SysUtils;

procedure puts(s: pchar); cdecl; external;
var errno_sim: integer; cvar; external 'c' name 'errno';

type
  PInt = ^integer;
  PByte = ^byte;
  TVarRec = record
    case kind: integer of
      0: (i: integer);
      1: (b: byte);
  end;

var
  base: integer;
  aliasBase: integer absolute base;

procedure TakeUntyped(var x);
var
  k: integer;
begin
  k := integer(x);
  Write(k);
  unsafe
    k := integer(x);
  end;
  Write(k);
end;

procedure Exercise;
var
  arr: array[0..3] of integer;
  a, b: array[0..3] of byte;
  p: PInt;
  pb: PByte;
  u: pointer;
  r: TVarRec;
  n: integer;
begin
  p := @arr[0];
  p := p + 1;
  Inc(p);
  n := p[0];
  pb := PByte(p);
  u := pointer(p);             { safe FFI conversion: no warning }
  GetMem(u, 8);
  ReallocMem(u, 16);
  Move(a, b, 4);
  FillChar(b, 4, 0);
  FreeMem(u);
  puts('hi');

  {$push}
  {$T+}
  {$pointermath checked}
  p := @arr[0];
  n := p[0];
  {$pop}

  unsafe
    u := @arr[0];
    p := p + 1;
    Inc(p);
    n := p[0];
    pb := PByte(p);
    GetMem(u, 8);
    ReallocMem(u, 16);
    Move(a, b, 4);
    FillChar(b, 4, 0);
    FreeMem(u);
    puts('ok');
  end;

  u := unsafe(@arr[0]);
  n := unsafe((p + 1)^);
  pb := unsafe(PByte(p));
  aliasBase := 1;
  r.kind := n;
  Writeln(r.kind + aliasBase, ' ', Ord(u <> nil) + Ord(pb <> nil));
end;

begin
  Exercise;
end.
