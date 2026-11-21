{ netasync demo — a concurrent TCP echo server.
 *
 * Everything happens in tasks: the main block opens a listener and spawns
 * the accept loop; the accept loop spawns one task per connection; each
 * connection task reads lines, echoes them back, and closes. When a task
 * would block in a syscall it parks instead (the OS thread goes on with
 * other tasks) — the whole server works on one worker per CPU core no
 * matter how many connections are open.
 *
 * Run it and connect with anything that speaks line-based TCP:
 *
 *   bin/netasync_demo                 several connections at once welcome
 *   bin/netasync_demo --requests 3    serve 3 connections, print, exit
 *
 *   nc 127.0.0.1 PORT                 (then type lines; Ctrl-D ends it)
 * }
{$H+}
program netasync_demo;

uses netasync, sysutils, threading;

const
  MaxLine = 4096;

var
  gServed: specialize TAtomic<Integer>;   { connections served so far }
  gRequests: Integer = 0;                { 0: serve until killed }

{ Bytes for a string, with CRLF at the end. }
function LineBytes(const s: string): TNetBytes;
begin
  SetLength(Result, Length(s) + 2);
  if Length(s) > 0 then
    move(pchar(s)^, Result[0], Length(s));
  Result[Length(s)] := 13;
  Result[Length(s) + 1] := 10;
end;

{ A connection task's exception goes here (detached tasks run on their
 * own; the server stays up however a client misbehaves). }
procedure ConnectionError(E: Exception);
begin
  Writeln('connection error: ', E.Message);
  Flush(output);
end;

{ Echoes one connection: lines until the connection ends. Runs as its own
 * task; the awaits park it whenever the socket is not ready, so the other
 * connections are served meanwhile. }
function HandleConnection(conn: TSocket; nth: Integer): async;
var
  pending: string;
  chunk: TNetBytes;
  i: Integer;
begin
  try
    pending := '';
    while true do
    begin
      chunk := await NetReadSome(conn, MaxLine);
      if Length(chunk) = 0 then
        break;                             { the connection ended }
      for i := 0 to High(chunk) do
      begin
        if chunk[i] = 10 then              { a whole line arrived }
        begin
          if copy(pending, Length(pending), 1) = #13 then
            pending := copy(pending, 1, Length(pending) - 1);
          Writeln('echo: ', pending);
          Flush(output);
          var sent := await NetSendAll(conn, LineBytes(pending));
          pending := '';
        end
        else
          pending := pending + char(chunk[i]);
      end;
    end;
  finally
    NetClose(conn);
    gServed.Increment;
  end;
end;

{ Accepts connections until --requests (if given) are served. }
function AcceptLoop(listener: TSocket): async;
var
  conn: TSocket;
  nth: Integer;
begin
  nth := 0;
  while (gRequests <= 0) or (nth < gRequests) do
  begin
    conn := await NetAccept(listener);     { parks until a client arrives }
    inc(nth);
    detached(ConnectionError) HandleConnection(conn, nth);
  end;
  Writeln('accept loop done (', nth, ' connections)');
  Flush(output);
end;

var
  listener: TSocket;

var
  i: Integer;
begin
  for i := 1 to ParamCount do
    if ParamStr(i) = '--requests' then
      gRequests := StrToIntDef(ParamStr(i + 1), 0);

  gServed := specialize TAtomic<Integer>.Create(0);
  listener := NetListen(0, 128, true);
  if listener = InvalidSocket then
  begin
    Writeln('could not listen');
    halt(1);
  end;
  Writeln('listening on port ', NetBoundPort(listener));
  Flush(output);

  scope
    spawn AcceptLoop(listener);
  end;

  { let the detached connection tasks finish before announcing it }
  while (gRequests > 0) and (gServed.Load < gRequests) do
    Sleep(10);

  Writeln('server stopped (', gServed.Load, ' connections served)');
  Flush(output);
end.
