{ netasync: an echo server and three clients at once, in one process.
  Server and clients are tasks; none of them blocks the OS thread in a
  syscall — reads, writes and the accept all park the task on the
  socket's readiness. Each task fills its own slot of a fixed array of
  shortstrings, and the main block prints the results in a fixed order
  after the scope has joined. Deterministic: no timing dependence.

  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t | diff - %S/netasync-echo.expected
  RUN: %p3 --mode=objfpc -O2 %s -o %t.O2
  RUN: %t.O2 | diff - %S/netasync-echo.expected
  }
{$mode objfpc}{$H+}
program netasync_echo;
uses netasync, sysutils;

const
  MSG = 32;

type
  TMsg = array[0..MSG - 1] of byte;

var
  gPort: Integer;
  gClients: array[1..3] of shortstring;
  gServer: shortstring;

function Padded(const s: string): TNetBytes;
var
  i: Integer;
begin
  SetLength(Result, MSG);
  for i := 0 to MSG - 1 do
    if i < Length(s) then
      Result[i] := Byte(s[i + 1])
    else
      Result[i] := Byte(' ');
end;

function Unpadded(const b: TNetBytes; n: Integer): string;
var
  i: Integer;
begin
  Result := '';
  for i := 0 to n - 1 do
    if b[i] <> Byte(' ') then
      Result := Result + char(b[i]);
end;

function EchoServer(listener: TSocket): async;
var
  conn: TSocket;
  got: TNetBytes;
  n, total, served, round: Integer;
begin
  SetLength(got, MSG);
  served := 0;
  for round := 1 to 3 do
  begin
    conn := await NetAccept(listener);
    total := 0;
    while total < MSG do
    begin
      n := await NetReadInto(conn, @got[total], MSG - total);
      if n = 0 then
        break;
      inc(total, n);
    end;
    var sent1 := await NetSendAll(conn, got);
    NetClose(conn);
    inc(served);
  end;
  gServer := 'echoed ' + IntToStr(served) + ' connections';
end;

function RoundTrip(nth: Integer; const s: string): async;
var
  conn: TSocket;
  got: TNetBytes;
  n, off: Integer;
begin
  SetLength(got, MSG);
  conn := await NetConnect('127.0.0.1', gPort);
  var sent2 := await NetSendAll(conn, Padded(s));
  off := 0;
  while off < MSG do
  begin
    n := await NetReadInto(conn, @got[off], MSG - off);
    if n = 0 then
      break;
    inc(off, n);
  end;
  NetClose(conn);
  gClients[nth] := Unpadded(got, off);
end;

var
  listener: TSocket;
begin
  listener := NetListen(0, 16, true);
  gPort := NetBoundPort(listener);
  scope
    spawn EchoServer(listener);
    spawn RoundTrip(1, 'one');
    spawn RoundTrip(2, '2nd');
    spawn RoundTrip(3, 'three!');
  end;
  NetClose(listener);
  Writeln(gServer);
  Writeln('client 1: ', gClients[1]);
  Writeln('client 2: ', gClients[2]);
  Writeln('client 3: ', gClients[3]);
  if gPort <= 0 then
    Writeln('no port');
end.
