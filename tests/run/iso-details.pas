{ ISO 7185 details found by the Pascal-P5 conformance test (iso7185pat):
  mixed comment delimiters, pointer binding to the current scope, dispose of
  an expression, new/dispose with variant selectors, arrays and records of
  files.
  RUN: %p3 %s -o %t
  RUN: %t > %t.out
  RUN: diff %S/iso-details.expected %t.out
}
program IsoDetails(output);
type intalias = integer;
     shape = (circle, square);
     ptr = ^rec;
     rec = record
       case kind: shape of
         circle: (radius: integer);
         square: (side: integer)
     end;
     logs = record name: char; f: text end;
var i: integer;
    r: ptr;
    files: array [1..3] of text;
    log: logs;
    c: char;

procedure bindToCurrentScope;
type pt = ^intalias;   { binds to the char below, not the outer integer }
     intalias = char;
var p: pt;
begin
  new(p); p^ := 'a'; write(p^); dispose(p)
end;

function make: ptr;
var q: ptr;
begin
  new(q, square); q^.kind := square; q^.side := 7; make := q
end;

begin
  i := 1;{ a brace comment closed ISO-style *)
  writeln(i);
  bindToCurrentScope; writeln;
  new(r, circle); r^.kind := circle; r^.radius := 5;
  writeln(r^.radius);
  dispose(r, circle);
  writeln(make^.side);
  dispose(make);              { dispose takes any pointer expression }
  for i := 1 to 3 do begin
    rewrite(files[i]); writeln(files[i], chr(ord('a') + i)); reset(files[i])
  end;
  for i := 3 downto 1 do begin read(files[i], c); write(c) end;
  writeln;
  rewrite(log.f); write(log.f, 'x'); reset(log.f); read(log.f, log.name);
  writeln(log.name)
end.
