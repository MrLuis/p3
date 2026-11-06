unit pcsys;
{ Operating-system access for the self-hosted compiler. Every C library
  call lives here (DIALECT.md section 3); all input and output goes
  through this unit, never through Pascal write/read (section 7). }

interface

type
  TCharArray = array[0..$7FFFFFF] of char;
  PCharArray = ^TCharArray;

var
  { C streams for standard output and standard error. }
  sysOut, sysErr: pointer;

procedure sysInit;

{ Output. Streams are buffered by the C library; sysFlush (and sysExit)
  write them out. }
procedure sysWrite(f: pointer; const s: string);
procedure sysWriteChar(f: pointer; c: char);
procedure sysWriteChars(f: pointer; p: PCharArray; start, count: int64);
procedure sysFlush;

{ Flushes the output streams and ends the program with the given status. }
procedure sysExit(code: longint);

{ Reads a whole file ('-': standard input) into a new buffer of len bytes
  followed by #0. Returns false if it cannot be opened or read. }
function sysReadFile(const path: string; var data: PCharArray;
                     var len: int64): boolean;
{ Writes count bytes of p to a file; false on failure. }
function sysWriteFile(const path: string; p: PCharArray; count: int64): boolean;
{ Why path cannot be read, as strerror would say it (errno is not portable
  to reach): missing, or present but unreadable. }
function sysOpenError(const path: string): string;
function sysFileExists(const path: string): boolean;
{ Does path name a file that is not a directory? }
function sysIsRegularFile(const path: string): boolean;
procedure sysRemoveFile(const path: string);

{ The environment variable name, if set. }
function sysGetEnv(const name: string; var value: string): boolean;
{ Runs a shell command; returns its exit status (-1 if it did not run). }
function sysRun(const cmd: string): longint;
{ Is file descriptor fd (1: stdout, 2: stderr) a terminal? }
function sysIsTerminal(fd: longint): boolean;
{ The double written in s (C strtod); ok is false if s is not a number. }
function sysStrToReal(const s: string; var ok: boolean): double;
{ +infinity (built from its bits: computing it would trap under FPC). }
function sysInfinity: double;

implementation

function c_fopen(path, mode: pchar): pointer; cdecl; external 'c' name 'fopen';
function c_fdopen(fd: longint; mode: pchar): pointer; cdecl; external 'c' name 'fdopen';
function c_fread(buf: pointer; size, n: int64; f: pointer): int64; cdecl; external 'c' name 'fread';
function c_fwrite(buf: pointer; size, n: int64; f: pointer): int64; cdecl; external 'c' name 'fwrite';
function c_fclose(f: pointer): longint; cdecl; external 'c' name 'fclose';
function c_fflush(f: pointer): longint; cdecl; external 'c' name 'fflush';
function c_remove(path: pchar): longint; cdecl; external 'c' name 'remove';
function c_getenv(name: pchar): pchar; cdecl; external 'c' name 'getenv';
function c_system(cmd: pchar): longint; cdecl; external 'c' name 'system';
function c_isatty(fd: longint): longint; cdecl; external 'c' name 'isatty';
function c_strtod(s: pchar; endp: pointer): double; cdecl; external 'c' name 'strtod';
function c_setvbuf(f: pointer; buf: pointer; mode: longint; size: int64): longint; cdecl;
  external 'c' name 'setvbuf';

const
  ioUnbuffered = 2; { _IONBF on glibc and macOS }

procedure sysInit;
var
  ignored: longint;
begin
  sysOut := c_fdopen(1, 'w');
  sysErr := c_fdopen(2, 'w');
  { diagnostics appear as they are reported, before any output written at
    the end (like llvm::errs) }
  ignored := c_setvbuf(sysErr, nil, ioUnbuffered, 0);
end;

procedure sysWrite(f: pointer; const s: string);
begin
  if length(s) > 0 then
    c_fwrite(@s[1], 1, length(s), f);
end;

procedure sysWriteChar(f: pointer; c: char);
begin
  c_fwrite(@c, 1, 1, f);
end;

procedure sysWriteChars(f: pointer; p: PCharArray; start, count: int64);
begin
  if count > 0 then
    {$R-}
    c_fwrite(@p^[start], 1, count, f);
    {$R+}
end;

procedure sysFlush;
begin
  c_fflush(sysOut);
  c_fflush(sysErr);
end;

procedure sysExit(code: longint);
begin
  sysFlush;
  halt(code);
end;

{ s with a terminating #0 in buf, for C. }
procedure toC(const s: string; var buf: string);
begin
  buf := s + #0;
end;

function sysReadFile(const path: string; var data: PCharArray;
                     var len: int64): boolean;
var
  cpath: string;
  f: pointer;
  cap, got: int64;
begin
  sysReadFile := false;
  data := nil;
  len := 0;
  toC(path, cpath);
  if path = '-' then
    f := c_fdopen(0, 'rb')
  else
    f := c_fopen(@cpath[1], 'rb');
  if f = nil then
    exit;
  cap := 65536;
  getmem(data, cap);
  repeat
    if len + 1 >= cap then
    begin
      cap := cap * 2;
      reallocmem(data, cap);
    end;
    {$R-}
    got := c_fread(@data^[len], 1, cap - len - 1, f);
    {$R+}
    len := len + got;
  until got = 0;
  if path <> '-' then
    c_fclose(f);
  {$R-}
  data^[len] := #0;
  {$R+}
  sysReadFile := true;
end;

function sysWriteFile(const path: string; p: PCharArray; count: int64): boolean;
var
  cpath: string;
  f: pointer;
  ok: boolean;
begin
  toC(path, cpath);
  f := c_fopen(@cpath[1], 'wb');
  if f = nil then
    exit(false);
  ok := (count = 0) or (c_fwrite(p, 1, count, f) = count);
  ok := (c_fclose(f) = 0) and ok;
  sysWriteFile := ok;
end;

function c_access(path: pchar; mode: longint): longint; cdecl; external 'c' name 'access';

function sysOpenError(const path: string): string;
var
  cpath: string;
begin
  toC(path, cpath);
  if c_access(@cpath[1], 0) <> 0 then
    sysOpenError := 'No such file or directory'
  else
    sysOpenError := 'Permission denied';
end;

function sysFileExists(const path: string): boolean;
var
  cpath: string;
  f: pointer;
begin
  toC(path, cpath);
  f := c_fopen(@cpath[1], 'rb');
  if f <> nil then
    c_fclose(f);
  sysFileExists := f <> nil;
end;

function c_opendir(path: pchar): pointer; cdecl; external 'c' name 'opendir';
function c_closedir(d: pointer): longint; cdecl; external 'c' name 'closedir';

function sysIsRegularFile(const path: string): boolean;
var
  cpath: string;
  d: pointer;
begin
  if not sysFileExists(path) then
    exit(false);
  toC(path, cpath);
  d := c_opendir(@cpath[1]);
  if d <> nil then
    c_closedir(d);
  sysIsRegularFile := d = nil;
end;

procedure sysRemoveFile(const path: string);
var
  cpath: string;
begin
  toC(path, cpath);
  c_remove(@cpath[1]);
end;

function sysGetEnv(const name: string; var value: string): boolean;
var
  cname: string;
  p: PCharArray;
  i: longint;
begin
  toC(name, cname);
  p := PCharArray(c_getenv(@cname[1]));
  value := '';
  if p = nil then
    exit(false);
  i := 0;
  {$R-}
  while (p^[i] <> #0) and (i < 255) do
  begin
    value := value + p^[i];
    i := i + 1;
  end;
  {$R+}
  sysGetEnv := true;
end;

function sysRun(const cmd: string): longint;
var
  ccmd: string;
  status: longint;
begin
  sysFlush; { keep our output ahead of the command's }
  toC(cmd, ccmd);
  status := c_system(@ccmd[1]);
  if status = -1 then
    exit(-1);
  sysRun := (status shr 8) and 255; { exit status of a normal exit }
end;

function sysIsTerminal(fd: longint): boolean;
begin
  sysIsTerminal := c_isatty(fd) <> 0;
end;

function sysInfinity: double;
type
  TOverlay = record
    case boolean of
      true: (d: double);
      false: (bits: int64);
  end;
var
  o: TOverlay;
begin
  o.bits := $7FF0000000000000;
  sysInfinity := o.d;
end;

function sysStrToReal(const s: string; var ok: boolean): double;
var
  cs: string;
  endp: PCharArray;
begin
  toC(s, cs);
  sysStrToReal := c_strtod(@cs[1], @endp);
  ok := (length(s) > 0) and (endp = PCharArray(@cs[length(s) + 1]));
end;

end.
