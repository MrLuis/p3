{ The PACKRECORDS directive (and ALIGN, A1..A32, A+ and A-; tp and objfpc),
  and packed records, as in Free Pascal: each field at a multiple of the
  smaller of its alignment and N, a variant part at a multiple of the
  smaller of N and 16, the size a multiple of the largest alignment used;
  C, DEFAULT and NORMAL (and A+) are the natural layout; packed and A- are
  1. Each record's bytes are dumped (through absolute) after its fields are
  set to markers; also nested records, arrays of records, field access,
  copies and var parameters through fields left unaligned, and typed
  constants. Output is identical to Free Pascal 3.2.2's (fpc -Mobjfpc and
  -Mtp).
  RUN: %pcc --mode=objfpc %s -o %t
  RUN: %t | diff - %S/packrecords.expected
  RUN: %pcc --mode=tp %s -o %t.tp
  RUN: %t.tp | diff - %S/packrecords.expected
  RUN: %pcc --mode=objfpc -O2 %s -o %t.O2
  RUN: %t.O2 | diff - %S/packrecords.expected
  RUN: %pcc --mode=objfpc -g %s -o %t.g
  RUN: %t.g | diff - %S/packrecords.expected
}
program packrecords;

type
  TBytes = array[0..95] of byte;

{$PACKRECORDS DEFAULT}
  TD = record a: byte; b: int64; c: word; d: longint; e: double; f: byte; g: array[0..2] of word; h: char; end;
  SD = record x: byte; r: TD; y: byte; end;
  VD = record k: byte; case integer of 0: (i: int64); 1: (w: word); end;
  WD = record a: word; case k: byte of 0: (c: byte); 1: (d: double; case integer of 0: (z: longint)); end;
{$PACKRECORDS 1}
  TP1 = record a: byte; b: int64; c: word; d: longint; e: double; f: byte; g: array[0..2] of word; h: char; end;
  SP1 = record x: byte; r: TP1; y: byte; end;
  VP1 = record k: byte; case integer of 0: (i: int64); 1: (w: word); end;
  WP1 = record a: word; case k: byte of 0: (c: byte); 1: (d: double; case integer of 0: (z: longint)); end;
{$PACKRECORDS 2}
  TP2 = record a: byte; b: int64; c: word; d: longint; e: double; f: byte; g: array[0..2] of word; h: char; end;
  SP2 = record x: byte; r: TP2; y: byte; end;
  VP2 = record k: byte; case integer of 0: (i: int64); 1: (w: word); end;
  WP2 = record a: word; case k: byte of 0: (c: byte); 1: (d: double; case integer of 0: (z: longint)); end;
{$PACKRECORDS 4}
  TP4 = record a: byte; b: int64; c: word; d: longint; e: double; f: byte; g: array[0..2] of word; h: char; end;
  SP4 = record x: byte; r: TP4; y: byte; end;
  VP4 = record k: byte; case integer of 0: (i: int64); 1: (w: word); end;
  WP4 = record a: word; case k: byte of 0: (c: byte); 1: (d: double; case integer of 0: (z: longint)); end;
{$PACKRECORDS 8}
  TP8 = record a: byte; b: int64; c: word; d: longint; e: double; f: byte; g: array[0..2] of word; h: char; end;
  SP8 = record x: byte; r: TP8; y: byte; end;
  VP8 = record k: byte; case integer of 0: (i: int64); 1: (w: word); end;
  WP8 = record a: word; case k: byte of 0: (c: byte); 1: (d: double; case integer of 0: (z: longint)); end;
{$PACKRECORDS 16}
  TP16 = record a: byte; b: int64; c: word; d: longint; e: double; f: byte; g: array[0..2] of word; h: char; end;
  SP16 = record x: byte; r: TP16; y: byte; end;
  VP16 = record k: byte; case integer of 0: (i: int64); 1: (w: word); end;
  WP16 = record a: word; case k: byte of 0: (c: byte); 1: (d: double; case integer of 0: (z: longint)); end;
{$PACKRECORDS 32}
  TP32 = record a: byte; b: int64; c: word; d: longint; e: double; f: byte; g: array[0..2] of word; h: char; end;
  SP32 = record x: byte; r: TP32; y: byte; end;
  VP32 = record k: byte; case integer of 0: (i: int64); 1: (w: word); end;
  WP32 = record a: word; case k: byte of 0: (c: byte); 1: (d: double; case integer of 0: (z: longint)); end;
{$PACKRECORDS C}
  TPC = record a: byte; b: int64; c: word; d: longint; e: double; f: byte; g: array[0..2] of word; h: char; end;
  SPC = record x: byte; r: TPC; y: byte; end;
  VPC = record k: byte; case integer of 0: (i: int64); 1: (w: word); end;
  WPC = record a: word; case k: byte of 0: (c: byte); 1: (d: double; case integer of 0: (z: longint)); end;
{$PACKRECORDS NORMAL}
  TPN = record a: byte; b: int64; c: word; d: longint; e: double; f: byte; g: array[0..2] of word; h: char; end;
  SPN = record x: byte; r: TPN; y: byte; end;
  VPN = record k: byte; case integer of 0: (i: int64); 1: (w: word); end;
  WPN = record a: word; case k: byte of 0: (c: byte); 1: (d: double; case integer of 0: (z: longint)); end;
{$PACKRECORDS DEFAULT}
  TPacked = packed record a: byte; b: longint; c: word; end;
  TInPacked = record x: byte; p: TPacked; y: word; end;
{$A1}  TA1 = record a: byte; b: longint; end;
{$A2}  TA2 = record a: byte; b: longint; end;
{$A+}  TA3 = record a: byte; b: int64; end;
{$A-}  TA4 = record a: byte; b: longint; end;
{$ALIGN 4}  TA5 = record a: byte; b: int64; end;
{$ALIGN ON}  TA6 = record a: byte; b: int64; end;
{$ALIGN OFF}  TA7 = record a: byte; b: int64; end;
{$PACKRECORDS 2}
  TInner2 = record a: byte; b: int64; end;
  TOuter2 = record x: byte; i: TInner2; arr: array[1..2] of TInner2; end;
{$PACKRECORDS 1}
  TOne = record a: byte; b: int64; c: double; d: array[1..2] of longint; r: TInner2; s: string[5]; end;
  TConstRec = record a: byte; b: longint; c: double; s: string[3]; t: array[1..2] of word; end;
{$PACKRECORDS DEFAULT}

const
  KC: TConstRec = (a: 7; b: 100000; c: 2.5; s: 'ab'; t: (513, 1027));
  KP: TPacked = (a: 1; b: -2; c: 3);

var
  p1: TOne;
  p1b: TBytes absolute p1;
  o2: TOuter2;
  o2b: TBytes absolute o2;
  kcb: TBytes absolute KC;

  vtD: TD; vtbD: TBytes absolute vtD; vsD: SD; vsbD: TBytes absolute vsD;
  vtP1: TP1; vtbP1: TBytes absolute vtP1; vsP1: SP1; vsbP1: TBytes absolute vsP1;
  vtP2: TP2; vtbP2: TBytes absolute vtP2; vsP2: SP2; vsbP2: TBytes absolute vsP2;
  vtP4: TP4; vtbP4: TBytes absolute vtP4; vsP4: SP4; vsbP4: TBytes absolute vsP4;
  vtP8: TP8; vtbP8: TBytes absolute vtP8; vsP8: SP8; vsbP8: TBytes absolute vsP8;
  vtP16: TP16; vtbP16: TBytes absolute vtP16; vsP16: SP16; vsbP16: TBytes absolute vsP16;
  vtP32: TP32; vtbP32: TBytes absolute vtP32; vsP32: SP32; vsbP32: TBytes absolute vsP32;
  vtPC: TPC; vtbPC: TBytes absolute vtPC; vsPC: SPC; vsbPC: TBytes absolute vsPC;
  vtPN: TPN; vtbPN: TBytes absolute vtPN; vsPN: SPN; vsbPN: TBytes absolute vsPN;

procedure Dump(const b: TBytes; n: integer);
var
  i: integer;
begin
  for i := 0 to n - 1 do
    write(b[i], ' ');
  writeln;
end;

procedure Bump(var x: int64);
begin
  x := x + 1;
end;

procedure Twice(var x: longint);
begin
  x := x * 2;
end;

begin
  writeln('DEFAULT ', SizeOf(TD), ' ', SizeOf(SD), ' ', SizeOf(VD), ' ', SizeOf(WD));
  fillchar(vtD, SizeOf(vtD), 0);
  vtD.a := 1; vtD.b := 2; vtD.c := 3; vtD.d := 4; vtD.e := 0; vtD.f := 6; vtD.g[0] := 7; vtD.g[2] := 8; vtD.h := 'A';
  Dump(vtbD, SizeOf(vtD));
  fillchar(vsD, SizeOf(vsD), 0);
  vsD.x := 9; vsD.r := vtD; vsD.y := 10;
  Dump(vsbD, SizeOf(vsD));
  writeln('1 ', SizeOf(TP1), ' ', SizeOf(SP1), ' ', SizeOf(VP1), ' ', SizeOf(WP1));
  fillchar(vtP1, SizeOf(vtP1), 0);
  vtP1.a := 1; vtP1.b := 2; vtP1.c := 3; vtP1.d := 4; vtP1.e := 0; vtP1.f := 6; vtP1.g[0] := 7; vtP1.g[2] := 8; vtP1.h := 'A';
  Dump(vtbP1, SizeOf(vtP1));
  fillchar(vsP1, SizeOf(vsP1), 0);
  vsP1.x := 9; vsP1.r := vtP1; vsP1.y := 10;
  Dump(vsbP1, SizeOf(vsP1));
  writeln('2 ', SizeOf(TP2), ' ', SizeOf(SP2), ' ', SizeOf(VP2), ' ', SizeOf(WP2));
  fillchar(vtP2, SizeOf(vtP2), 0);
  vtP2.a := 1; vtP2.b := 2; vtP2.c := 3; vtP2.d := 4; vtP2.e := 0; vtP2.f := 6; vtP2.g[0] := 7; vtP2.g[2] := 8; vtP2.h := 'A';
  Dump(vtbP2, SizeOf(vtP2));
  fillchar(vsP2, SizeOf(vsP2), 0);
  vsP2.x := 9; vsP2.r := vtP2; vsP2.y := 10;
  Dump(vsbP2, SizeOf(vsP2));
  writeln('4 ', SizeOf(TP4), ' ', SizeOf(SP4), ' ', SizeOf(VP4), ' ', SizeOf(WP4));
  fillchar(vtP4, SizeOf(vtP4), 0);
  vtP4.a := 1; vtP4.b := 2; vtP4.c := 3; vtP4.d := 4; vtP4.e := 0; vtP4.f := 6; vtP4.g[0] := 7; vtP4.g[2] := 8; vtP4.h := 'A';
  Dump(vtbP4, SizeOf(vtP4));
  fillchar(vsP4, SizeOf(vsP4), 0);
  vsP4.x := 9; vsP4.r := vtP4; vsP4.y := 10;
  Dump(vsbP4, SizeOf(vsP4));
  writeln('8 ', SizeOf(TP8), ' ', SizeOf(SP8), ' ', SizeOf(VP8), ' ', SizeOf(WP8));
  fillchar(vtP8, SizeOf(vtP8), 0);
  vtP8.a := 1; vtP8.b := 2; vtP8.c := 3; vtP8.d := 4; vtP8.e := 0; vtP8.f := 6; vtP8.g[0] := 7; vtP8.g[2] := 8; vtP8.h := 'A';
  Dump(vtbP8, SizeOf(vtP8));
  fillchar(vsP8, SizeOf(vsP8), 0);
  vsP8.x := 9; vsP8.r := vtP8; vsP8.y := 10;
  Dump(vsbP8, SizeOf(vsP8));
  writeln('16 ', SizeOf(TP16), ' ', SizeOf(SP16), ' ', SizeOf(VP16), ' ', SizeOf(WP16));
  fillchar(vtP16, SizeOf(vtP16), 0);
  vtP16.a := 1; vtP16.b := 2; vtP16.c := 3; vtP16.d := 4; vtP16.e := 0; vtP16.f := 6; vtP16.g[0] := 7; vtP16.g[2] := 8; vtP16.h := 'A';
  Dump(vtbP16, SizeOf(vtP16));
  fillchar(vsP16, SizeOf(vsP16), 0);
  vsP16.x := 9; vsP16.r := vtP16; vsP16.y := 10;
  Dump(vsbP16, SizeOf(vsP16));
  writeln('32 ', SizeOf(TP32), ' ', SizeOf(SP32), ' ', SizeOf(VP32), ' ', SizeOf(WP32));
  fillchar(vtP32, SizeOf(vtP32), 0);
  vtP32.a := 1; vtP32.b := 2; vtP32.c := 3; vtP32.d := 4; vtP32.e := 0; vtP32.f := 6; vtP32.g[0] := 7; vtP32.g[2] := 8; vtP32.h := 'A';
  Dump(vtbP32, SizeOf(vtP32));
  fillchar(vsP32, SizeOf(vsP32), 0);
  vsP32.x := 9; vsP32.r := vtP32; vsP32.y := 10;
  Dump(vsbP32, SizeOf(vsP32));
  writeln('C ', SizeOf(TPC), ' ', SizeOf(SPC), ' ', SizeOf(VPC), ' ', SizeOf(WPC));
  fillchar(vtPC, SizeOf(vtPC), 0);
  vtPC.a := 1; vtPC.b := 2; vtPC.c := 3; vtPC.d := 4; vtPC.e := 0; vtPC.f := 6; vtPC.g[0] := 7; vtPC.g[2] := 8; vtPC.h := 'A';
  Dump(vtbPC, SizeOf(vtPC));
  fillchar(vsPC, SizeOf(vsPC), 0);
  vsPC.x := 9; vsPC.r := vtPC; vsPC.y := 10;
  Dump(vsbPC, SizeOf(vsPC));
  writeln('NORMAL ', SizeOf(TPN), ' ', SizeOf(SPN), ' ', SizeOf(VPN), ' ', SizeOf(WPN));
  fillchar(vtPN, SizeOf(vtPN), 0);
  vtPN.a := 1; vtPN.b := 2; vtPN.c := 3; vtPN.d := 4; vtPN.e := 0; vtPN.f := 6; vtPN.g[0] := 7; vtPN.g[2] := 8; vtPN.h := 'A';
  Dump(vtbPN, SizeOf(vtPN));
  fillchar(vsPN, SizeOf(vsPN), 0);
  vsPN.x := 9; vsPN.r := vtPN; vsPN.y := 10;
  Dump(vsbPN, SizeOf(vsPN));
  writeln(SizeOf(TPacked), ' ', SizeOf(TInPacked), ' ', SizeOf(TA1), ' ', SizeOf(TA2), ' ', SizeOf(TA3), ' ', SizeOf(TA4), ' ', SizeOf(TA5), ' ', SizeOf(TA6), ' ', SizeOf(TA7));
  writeln(SizeOf(TInner2), ' ', SizeOf(TOuter2), ' ', SizeOf(TOne));
  fillchar(o2, SizeOf(o2), 0);
  o2.x := 1; o2.i.a := 2; o2.i.b := 3; o2.arr[2].b := 4; o2.arr[1] := o2.i;
  Dump(o2b, SizeOf(o2));
  fillchar(p1, SizeOf(p1), 0);
  p1.a := 5; p1.b := 1000000000000; p1.c := 1.5; p1.d[2] := 77; p1.r.b := 9; p1.s := 'hey';
  Bump(p1.b);
  Twice(p1.d[2]);
  p1.r := o2.arr[2];
  writeln(p1.b, ' ', p1.c:0:2, ' ', p1.d[2], ' ', p1.r.b, ' ', p1.s, ' ', length(p1.s));
  Dump(p1b, SizeOf(p1));
  writeln(SizeOf(TConstRec), ' ', KC.a, ' ', KC.b, ' ', KC.c:0:2, ' ', KC.s, ' ', KC.t[2], ' ', KP.b, ' ', KP.c);
  Dump(kcb, SizeOf(KC));
end.
