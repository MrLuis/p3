program errors;
const
  lib = 'c';
  num = 42;
type
  TRec = record a: longint; end;
  TPlain = function(a, b: pointer): longint;
  TCB = function(a, b: pointer): longint; cdecl;
  TBadCB = function(const s: string): longint; cdecl;
  TIntFn = function(x: longint): longint; cdecl;
  TTakesCB = procedure(cb: TCB); cdecl;
function f1: longint; cdecl; external num;
function f2: longint; cdecl; external lib name num;
function f3(x: longint): longint; varargs; external lib;
procedure p2(cb: TPlain); cdecl; external lib;
procedure p3(cb: TBadCB); cdecl; external lib;
procedure p4(cb: TCB); cdecl; external lib;
function printf(f: pchar): longint; cdecl; varargs; external lib;
function plain(x: longint): longint; external lib name 'abs';
function cabs(x: longint): longint; cdecl; external lib name 'abs';
function Plain2(a, b: pointer): longint;
begin Plain2 := 0 end;
procedure Outer;
  function Inner(a, b: pointer): longint; cdecl;
  begin Inner := 0 end;
begin
  p4(@Inner);
end;
var
  r: TRec;
  s: string;
  f: TIntFn;
  g: TTakesCB;
begin
  printf('%d', r);
  printf('%s', s);
  printf('%d %s', 1, 'ok');
  p4(@Plain2);
  f := @plain;
  f := @printf;
  f := @cabs;
  p4(@printf);
  g := @p4;
end.
