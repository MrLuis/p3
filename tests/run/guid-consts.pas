(* TGUID typed constants and initialized variables from GUID strings, and
  DWord, as in Free Pascal: '{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}'
  (hexadecimal digits, either case; Data1, Data2 and Data3 as numbers, Data4
  as bytes in order) or '' (all zeros), also through a named string constant
  and inside arrays and records. Output is identical to Free Pascal 3.2.2's
  (fpc -Mobjfpc).
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t | diff - %S/guid-consts.expected
  RUN: %p3 --mode=objfpc -O2 %s -o %t.O2
  RUN: %t.O2 | diff - %S/guid-consts.expected
  RUN: %p3 --mode=objfpc -g %s -o %t.g
  RUN: %t.g | diff - %S/guid-consts.expected
*)
program guidconsts;
{$H+}

uses sysutils;

const
  G1: TGUID = '{01234567-89AB-CDEF-0123-456789ABCDEF}';
  GS = '{11111111-2222-3333-4444-555555555555}';
  G2: TGUID = GS;
  Lower: TGUID = '{deadbeef-cafe-f00d-a1b2-c3d4e5f60718}';
  Empty: TGUID = '';
  List: array[0..1] of TGUID = ('{00000000-0000-0000-0000-000000000001}',
                                '{a0000000-0000-0000-0000-00000000000f}');

type
  TEntry = record
    id: TGUID;
    n: longint;
  end;

const
  Entry: TEntry = (id: '{00000000-0000-0000-0000-0000000000AB}'; n: 3);

var
  v: TGUID = '{FFFFFFFF-0000-0000-0000-000000000000}';
  d: DWord;
  pd: ^DWord;

procedure Show(const g: TGUID);
var
  i: longint;
  s: string;
begin
  s := IntToHex(g.D1, 8) + '-' + IntToHex(g.D2, 4) + '-' + IntToHex(g.D3, 4) + '-';
  for i := 0 to 7 do
  begin
    s := s + IntToHex(g.D4[i], 2);
    if i = 1 then
      s := s + '-';
  end;
  writeln(s);
end;

procedure Local;
var
  g: TGUID = '{0F0F0F0F-1111-2222-3333-444444444444}';
begin
  Show(g);
  g.D1 := 0; { set again on the next call }
end;

begin
  Show(G1);
  writeln(G1.D4[0], ' ', G1.D4[7], ' ', G1.time_low, ' ', G1.clock_seq_low,
    ' ', G1.node[5]);
  Show(G2);
  Show(Lower);
  Show(Empty);
  Show(List[0]);
  Show(List[1]);
  Show(Entry.id);
  writeln(Entry.n);
  Show(v);
  Local;
  Local;
  d := 4000000000;
  pd := @d;
  writeln(d, ' ', SizeOf(DWord), ' ', pd^ + 1, ' ', High(DWord));
end.
