{ SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception }
program d03_ffi;
{ Dialect check L5: external C functions (cdecl), pchar, string literals
  passed as pchar, and a C call that runs a subprocess (L11). }
type
  TCharArray = array[0..$FFFFFFF] of char;
  PCharArray = ^TCharArray;

function c_strlen(s: pchar): int64; cdecl; external 'c' name 'strlen';
function c_getenv(name: pchar): pchar; cdecl; external 'c' name 'getenv';
function c_system(cmd: pchar): longint; cdecl; external 'c' name 'system';
function c_strtod(s: pchar; endp: pointer): double; cdecl; external 'c' name 'strtod';
function c_fopen(path, mode: pchar): pointer; cdecl; external 'c' name 'fopen';
function c_fwrite(buf: pointer; size, n: int64; f: pointer): int64; cdecl; external 'c' name 'fwrite';
function c_fread(buf: pointer; size, n: int64; f: pointer): int64; cdecl; external 'c' name 'fread';
function c_fclose(f: pointer): longint; cdecl; external 'c' name 'fclose';
function c_remove(path: pchar): longint; cdecl; external 'c' name 'remove';

{ Appends a NUL so the string's characters can be handed to C. }
function cstr(var s: string): pchar;
begin
  s := s + #0;
  cstr := @s[1];
end;

var
  s, path: string;
  f: pointer;
  buf: array[0..63] of char;
  got: int64;
  i: longint;
  pc: PCharArray;
begin
  s := 'hello';
  writeln(c_strlen(cstr(s)));
  writeln(c_strlen('literal'));
  writeln(c_getenv('P3_DIALECT_NO_SUCH_VARIABLE') = nil);
  writeln(c_strtod('2.5e3', nil):0:1);
  { the low byte of the wait status is 0, the exit code is in bits 8..15 }
  writeln(c_system('exit 3') shr 8);
  path := 'd03_ffi.tmp';
  f := c_fopen(cstr(path), 'wb');
  s := 'written by fwrite';
  writeln(c_fwrite(@s[1], 1, length(s), f));
  c_fclose(f);
  f := c_fopen(@path[1], 'rb');
  got := c_fread(@buf[0], 1, sizeof(buf), f);
  c_fclose(f);
  c_remove(@path[1]);
  pc := PCharArray(@buf[0]);
  {$R-}
  for i := 0 to got - 1 do
    write(pc^[i]);
  {$R+}
  writeln(' (', got, ')');
end.
