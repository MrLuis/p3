{ echo.pas: the netasync echo server of the benchmark — one task per
  connection, NetReadInto then NetSendPtr, the shape of an HTTP keep-alive
  handler. Run with P3_POOL_THREADS=N for N pool threads.

    echo PORT }
program echo;
uses netasync, sysutils;

function Serve(conn: TSocket): async;
var
  buf: array[0..65535] of byte;
  n: LongInt;
begin
  NetSetNoDelay(conn);
  try
    while true do
    begin
      n := await NetReadInto(conn, @buf, sizeof(buf));
      if n <= 0 then
        break;
      await NetSendPtr(conn, @buf, n);
    end;
  except
    on ENetError do ;
  end;
  NetClose(conn);
end;

{ Each connection goes to the thread pool (Pool.Run): a task started any
  other way, `detached` included, stays on the thread that started it —
  here the acceptor's — and the server would run on one thread. }
function Launch(conn: TSocket): async;
begin
  await Pool.Run(Serve(conn));
end;

function Acceptor(listener: TSocket): async;
begin
  while true do
    detached Launch(await NetAccept(listener));
end;

var
  opt: TNetListenOpts;
  lst: TSocket;
begin
  opt.ReuseAddr := true;
  opt.ReusePort := false;
  lst := NetListenTo('127.0.0.1', StrToInt(ParamStr(1)), 4096, opt);
  Wait(Acceptor(lst));
end.
