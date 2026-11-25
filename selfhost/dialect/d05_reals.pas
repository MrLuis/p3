{ SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception }
program d05_reals;
{ Dialect check L9: reinterpreting a double's bits through a variant
  record (needed to print LLVM hex float constants). }
type
  TOverlay = record
    case boolean of
      true:  (r: double);
      false: (bits: int64);
  end;

function hex64(v: int64): string;
const
  digits: array[0..15] of char = '0123456789ABCDEF';
var
  s: string;
  i: longint;
begin
  s := '';
  for i := 1 to 16 do
  begin
    s := digits[v and 15] + s;
    v := v shr 4;
  end;
  hex64 := s;
end;

var
  o: TOverlay;
begin
  o.r := 0.1;
  writeln('0x', hex64(o.bits));
  o.r := -2.0;
  writeln('0x', hex64(o.bits));
  o.bits := $3FF0000000000000;
  writeln(o.r:0:3);
end.
