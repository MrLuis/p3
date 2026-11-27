{ Concurrent I/O from pool threads: shared text file writes, concurrent
  open/write/read/close of files (__pas_open_files), and 64-bit integer reads
  under both POSIX pthreads and Windows CRITICAL_SECTION locking.
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t %t.data | diff - %S/io-threads.expected
  RUN: %p3 --mode=objfpc -O2 %s -o %t.fast
  RUN: %t.fast %t.data2 | diff - %S/io-threads.expected
}
{$H+}
program iothreads;

uses SysUtils, threading;

const
  NTasks = 8;
  NLines = 250;

var
  sharedFile: text;
  basePath: string;

function Worker(tid: longint): async int64;
var
  i: longint;
  localFile: text;
  path: string;
  sum: int64;
  i64: int64;
  u64: qword;
begin
  path := basePath + '.' + IntToStr(tid) + '.txt';
  assign(localFile, path);
  rewrite(localFile);
  for i := 1 to 20 do
  begin
    writeln(localFile, -i * 100000000000, ' ', qword(18000000000000000000) + qword(i + tid));
    writeln(sharedFile, tid, ':', i, ':', tid * 1000 + i);
  end;
  close(localFile);

  reset(localFile);
  sum := 0;
  for i := 1 to 20 do
  begin
    readln(localFile, i64, u64);
    if i64 <> -int64(i) * 100000000000 then
    begin
      sum := -1;
      break;
    end;
    if u64 <> qword(18000000000000000000) + qword(i + tid) then
    begin
      sum := -2;
      break;
    end;
    sum := sum + i;
  end;
  close(localFile);

  for i := 21 to NLines do
    writeln(sharedFile, tid, ':', i, ':', tid * 1000 + i);

  Result := sum;
end;

var
  tasks: array of async int64;
  results: array of int64;
  counts: array[0..NTasks - 1] of longint;
  seqSums: array[0..NTasks - 1] of int64;
  i, p1, p2, tid, seq, chk: longint;
  line: string;
  ok: boolean;
  totalLines: longint;
begin
  basePath := ParamStr(1);
  assign(sharedFile, basePath + '.shared.txt');
  rewrite(sharedFile);

  SetLength(tasks, NTasks);
  for i := 0 to NTasks - 1 do
    tasks[i] := Pool.Run(Worker(i));
  results := Wait(WhenAll(tasks));
  close(sharedFile);

  ok := true;
  for i := 0 to NTasks - 1 do
  begin
    if results[i] <> 210 then
      ok := false;
    counts[i] := 0;
    seqSums[i] := 0;
  end;

  reset(sharedFile);
  totalLines := 0;
  while not eof(sharedFile) do
  begin
    readln(sharedFile, line);
    p1 := Pos(':', line);
    if p1 <= 1 then
    begin
      ok := false;
      break;
    end;
    tid := StrToIntDef(Copy(line, 1, p1 - 1), -1);
    Delete(line, 1, p1);
    p2 := Pos(':', line);
    if p2 <= 1 then
    begin
      ok := false;
      break;
    end;
    seq := StrToIntDef(Copy(line, 1, p2 - 1), -1);
    chk := StrToIntDef(Copy(line, p2 + 1, Length(line)), -1);
    if (tid < 0) or (tid >= NTasks) or (seq < 1) or (seq > NLines) or
       (chk <> tid * 1000 + seq) then
    begin
      ok := false;
      break;
    end;
    counts[tid] := counts[tid] + 1;
    seqSums[tid] := seqSums[tid] + seq;
    totalLines := totalLines + 1;
  end;
  close(sharedFile);

  for i := 0 to NTasks - 1 do
    if (counts[i] <> NLines) or (seqSums[i] <> NLines * (NLines + 1) div 2) then
      ok := false;

  if ok then
    Writeln('ok ', totalLines)
  else
    Writeln('fail ', totalLines);
end.
