{ netloop: a line-echo server on 2 loops, its clients netasync tasks of the
  same process — two lines in one write, a line in two writes, a 3 MB answer
  (writability, back-pressure), Close after an answer, the peer's
  half-close, the idle timeout, 20 clients at once, and Stop closing all.
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t | diff - %S/netloop-echo.expected
  RUN: %p3 --mode=objfpc -O2 %s -o %t.O2
  RUN: %t.O2 | diff - %S/netloop-echo.expected
  }
{$mode objfpc}{$H+}
program netloop_echo;
uses sysutils, threading, netasync, netloop;

type
  TCount = specialize TAtomic<Integer>;

var
  Srv: TNetLoopServer;
  Opened, Closed, Lines: TCount;
  Port: Integer;

function Repeated(c: Char; n: Integer): string;
begin
  SetLength(Result, n);
  if n > 0 then
    FillChar(Result[1], n, c);
end;

{ ---- the server: every whole line back, "echo: " before it ---- }

procedure OnData(C: TLoopConn);
var
  text, line: string;
  p, used: Integer;
begin
  text := C.InText;
  used := 0;
  while true do
  begin
    p := Pos(#10, Copy(text, used + 1, Length(text)));
    if p = 0 then
      break;
    line := Copy(text, used + 1, p - 1);
    used := used + p;
    Lines.Increment;
    if line = 'big' then
      C.Write(Repeated('x', 3 * 1024 * 1024) + #10)
    else
      C.Write('echo: ' + line + #10);
    if line = 'quit' then
      C.Close;
  end;
  C.Consume(used);
end;

function RunServer: async;
begin
  Srv.Run;
end;

{ ---- clients ---- }

function ReadBytes(s: TSocket; n: Integer): string;
var
  buf: array[0..65535] of byte;
  got: LongInt;
  chunk: string;
begin
  Result := '';
  while Length(Result) < n do
  begin
    got := Wait(NetReadInto(s, @buf, sizeof(buf)));
    if got <= 0 then
      break;
    SetLength(chunk, got);
    Move(buf[0], chunk[1], got);
    Result := Result + chunk;
  end;
end;

procedure SendText(s: TSocket; const t: string);
begin
  Wait(NetSendPtr(s, PChar(t), Length(t)));
end;

function Connect: TSocket;
begin
  Result := Wait(NetConnect('127.0.0.1', Port));
end;

function Chatty(n: Integer): async Integer;
var
  s: TSocket;
  i: Integer;
  want: string;
begin
  s := await NetConnect('127.0.0.1', Port);
  Result := 0;
  for i := 1 to n do
  begin
    SendText(s, 'm' + IntToStr(i) + #10);
    want := 'echo: m' + IntToStr(i) + #10;
    if ReadBytes(s, Length(want)) = want then
      inc(Result);
  end;
  NetClose(s);
end;

var
  srvTask: async;
  s: TSocket;
  got: string;
  t: array[1..20] of async Integer;
  i, total: Integer;
  t0: Int64;
begin
  Opened := TCount.Create(0);
  Closed := TCount.Create(0);
  Lines := TCount.Create(0);
  Srv := TNetLoopServer.Create(2);
  Srv.IdleTimeoutMs := 400;
  Srv.Listen('127.0.0.1', 0);
  Port := Srv.BoundPort;
  Srv.OnOpen := procedure(C: TLoopConn) => Opened.Increment;
  Srv.OnClose := procedure(C: TLoopConn) => Closed.Increment;
  Srv.OnData := @OnData;
  srvTask := Pool.Run(RunServer);

  s := Connect;
  SendText(s, 'hello' + #10 + 'world' + #10);
  got := ReadBytes(s, 24);
  Writeln('two lines in one write: ', got = 'echo: hello' + #10 + 'echo: world' + #10);
  SendText(s, 'par');
  Sleep(50);
  SendText(s, 'tial' + #10);
  Writeln('a line in two writes: ', ReadBytes(s, 14) = 'echo: partial' + #10);
  SendText(s, 'big' + #10);
  got := ReadBytes(s, 3 * 1024 * 1024 + 1);
  Writeln('3 MB answer: ', Length(got), ' bytes, all x: ',
          got = Repeated('x', 3 * 1024 * 1024) + #10);
  SendText(s, 'quit' + #10);
  got := ReadBytes(s, 1000);   { the echo, then the end: the server closed }
  Writeln('Close after the answer: ', got = 'echo: quit' + #10);
  NetClose(s);

  s := Connect;
  SendText(s, 'last' + #10);
  NetShutdownWrite(s);
  Writeln('half-close, answered then closed: ', ReadBytes(s, 1000) = 'echo: last' + #10);
  NetClose(s);

  s := Connect;
  t0 := GetTickCount64;
  got := ReadBytes(s, 1);       { nothing comes: the idle timeout closes it }
  Writeln('idle timeout: closed, after 0.4-2.5 s: ',
          (got = '') and (GetTickCount64 - t0 >= 400) and (GetTickCount64 - t0 < 2500));
  NetClose(s);

  for i := 1 to 20 do
    t[i] := Chatty(50);
  total := 0;
  for i := 1 to 20 do
    total := total + Wait(t[i]);
  Writeln('20 clients x 50 round trips: ', total);

  s := Connect;                  { left open: Stop closes it }
  SendText(s, 'ping' + #10);
  ReadBytes(s, 11);
  Srv.Stop;
  Wait(srvTask);
  Writeln('after Stop: the server closed it: ', ReadBytes(s, 1) = '');
  NetClose(s);
  Writeln('connections opened ', Opened.Load, ', closed ', Closed.Load, ', lines ', Lines.Load);
  Srv.Free;
end.
