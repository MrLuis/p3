{ Concurrent __pas_sys_strerror calls after failed fopen from multiple
  Pool.Run threads on POSIX (strerror_r) and Windows (strerror_s).
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t %t.missing | diff - %S/sys-strerror-threads.expected
  RUN: %p3 --mode=objfpc -O2 %s -o %t.fast
  RUN: %t.fast %t.missing | diff - %S/sys-strerror-threads.expected
}
{$H+}
program sysstrerrorthreads;

uses SysUtils, threading;

const
  NTasks = 8;
  NIters = 200;

var
  missingBase: string;

function c_fopen(path, mode: pchar): pointer; cdecl; external name 'fopen';
function c_fclose(f: pointer): longint; cdecl; external name 'fclose';
function sys_strerror: pchar; cdecl; external name '__pas_sys_strerror';

function Worker(tid: longint): async longint;
var
  i, okCount: longint;
  path, msg: string;
  f: pointer;
begin
  okCount := 0;
  for i := 1 to NIters do
  begin
    path := missingBase + '.' + IntToStr(tid) + '.' + IntToStr(i);
    f := c_fopen(pchar(path), 'rb');
    if f <> nil then
      c_fclose(f)
    else
    begin
      msg := sys_strerror;
      if Length(msg) > 0 then
        okCount := okCount + 1;
    end;
  end;
  Result := okCount;
end;

var
  tasks: array of async longint;
  results: array of longint;
  i, total: longint;
begin
  missingBase := ParamStr(1);
  SetLength(tasks, NTasks);
  for i := 0 to NTasks - 1 do
    tasks[i] := Pool.Run(Worker(i));
  results := Wait(WhenAll(tasks));
  total := 0;
  for i := 0 to NTasks - 1 do
    total := total + results[i];
  Writeln('ok ', total);
end.
