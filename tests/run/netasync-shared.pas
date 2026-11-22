{ netasync: several tasks parked on one listener fd at the same time —
  one serving accepts, one extra waiter that only observes readiness and
  exits after its first wake (retiring must not take the kernel interest
  away from the still-parked accept task, and its own wake must arrive
  reporting the READ bit) — plus two sockets bound to one port through
  SO_REUSEPORT, a pointer/count send, keepalive parameters, and a
  half-close (SHUT_WR) that ends the peer's read with EOF after the
  response.

  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t | diff - %S/netasync-shared.expected
  RUN: %p3 --mode=objfpc -O2 %s -o %t.O2
  RUN: %t.O2 | diff - %S/netasync-shared.expected
  }
{$mode objfpc}{$H+}
program netasync_shared;
uses netasync, sysutils;

const
  MSG = 16;

var
  gPort: Integer;
  gClients: array[1..4] of shortstring;
  gServed, gExtraParks: Integer;
  gExtraBits: boolean = true;
  gKeepAlive: boolean = false;
  gShutOk: boolean = true;
  gUdpPort: Integer;
  gUdp1, gUdp2: shortstring;
  gWaitOk: boolean = true;

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

{ The listener's serving task: four accepts (three from literals, one
  from a resolved name). }
function Served(listener: TSocket): async;
var
  conn: TSocket;
  buf: TNetBytes;
  n, total, round: Integer;
begin
  SetLength(buf, MSG);
  for round := 1 to 4 do
  begin
    conn := await NetAccept(listener);
    total := 0;
    repeat
      var got := await NetReadInto(conn, @buf[total], MSG - total);
      n := got;
      if n = 0 then
        break;
      inc(total, n);
    until total >= MSG;
    var sent := await NetSendPtr(conn, @buf[0], MSG);
    NetClose(conn);
    inc(gServed);
  end;
end;

{ A waiter parked beside the accept task: parks once via NetWaitFd and
  exits; the wake must report the READ bit. }
function ExtraWaiter(listener: TSocket): async;
var
  bits: Integer;
begin
  bits := NetWaitFd(listener, true, false);
  inc(gExtraParks);
  if (bits and NW_READ) = 0 then
    gExtraBits := false;
end;

{ Three deadlines on a quiet socket must expire (NW_TIMEOUT, neither
  leaked nor lost), then a self-datagram must pre-empt a far deadline. }
function TimedWaiter(q: TSocket): async;
var
  buf: TNetBytes;
  bits, n: Integer;
  t0: Int64;
  i: Integer;
begin
  SetLength(buf, 16);
  for i := 1 to 3 do
  begin
    t0 := GetTickCount64;
    bits := NetWaitFdUntil(q, true, false, 25);
    n := Integer(GetTickCount64 - t0);
    if (bits <> NW_TIMEOUT) or (n < 24) or (n > 500) then
      gWaitOk := false;
  end;
  NetUdpPeerTo(q, '127.0.0.1', NetBoundPort(q));
  SetLength(buf, 8);
  buf[0] := Byte('t'); buf[1] := Byte('i'); buf[2] := Byte('c'); buf[3] := Byte('k');
  var sent := await NetUdpSend(q, @buf[0], 4);
  t0 := GetTickCount64;
  bits := NetWaitFdUntil(q, true, false, 400);
  n := Integer(GetTickCount64 - t0);
  if (bits <> NW_READ) or (n > 50) then
    gWaitOk := false;
  NetClose(q);
end;

function Client(nth: Integer; const Host, s: string): async;
var
  conn: TSocket;
  got: TNetBytes;
  n, off: Integer;
begin
  SetLength(got, MSG);
  conn := await NetConnect(Host, gPort);
  var sent := await NetSendAll(conn, Padded(s));
  off := 0;
  repeat
    var part := await NetReadInto(conn, @got[off], MSG - off);
    n := part;
    if n = 0 then
      break;
    inc(off, n);
  until off >= MSG;
  NetClose(conn);
  gClients[nth] := Unpadded(got, off);
end;

{ The UDP echo server: every datagram goes back to its source. The
  socket is bound by the main block (like the listener), so the client
  already has the port when the pool threads start. }
function UdpServed(u: TSocket): async;
var
  buf: TNetBytes;
  src: TSockAddrIn;
  n: Integer;
begin
  SetLength(buf, 16);
  for var round := 1 to 2 do
  begin
    var got := await NetUdpRecvFrom(u, @buf[0], 16, @src);
    n := got;
    var sent := await NetUdpSendTo(u, @src, @buf[0], n);
  end;
  NetClose(u);
end;

{ A UDP client: two datagrams to the connected socket, each echoed. }
function UdpClient: async;
var
  u: TSocket;
  buf, pad: TNetBytes;
  n, total: Integer;
begin
  SetLength(buf, 16);
  u := NetUdpConnect('127.0.0.1', gUdpPort);
  pad := Padded('alpha');
  var sent := await NetUdpSend(u, @pad[0], 8);
  total := 0;
  repeat
    var part := await NetUdpRecv(u, @buf[total], 16 - total);
    n := part;
    if n = 0 then
      break;
    inc(total, n);
  until total >= 8;
  gUdp1 := Unpadded(buf, 8);
  pad := Padded('beta');
  var sent2 := await NetUdpSend(u, @pad[0], 8);
  total := 0;
  repeat
    var part2 := await NetUdpRecv(u, @buf[total], 16 - total);
    n := part2;
    if n = 0 then
      break;
    inc(total, n);
  until total >= 8;
  gUdp2 := Unpadded(buf, 8);
  NetClose(u);
end;

{ The half-close scenario: the server answers 8 bytes, then SHUT_WR; the
  client reads the answer and the end of the connection (keepalive timing
  set on the connection along the way). }
function ShutServed(listener: TSocket; const s: string): async;
var
  conn: TSocket;
  buf: TNetBytes;
begin
  conn := await NetAccept(listener);
  NetSetKeepAlive(conn);
  NetSetKeepAliveTimer(conn, 300, 30, 5);
  gKeepAlive := true;
  SetLength(buf, 8);
  for var i := 0 to Length(s) - 1 do
    buf[i] := Byte(s[i + 1]);
  for var i := Length(s) to 7 do
    buf[i] := Byte(' ');
  var sent := await NetSendPtr(conn, @buf[0], 8);
  NetShutdownWrite(conn);
  NetClose(conn);
end;

function ShutClient(Port: Integer): async;
var
  conn: TSocket;
  buf: TNetBytes;
  n, total: Integer;
  eof: boolean;
begin
  SetLength(buf, 16);
  conn := await NetConnect('127.0.0.1', Port);
  total := 0;
  eof := false;
  repeat
    var part := await NetReadInto(conn, @buf[total], 16 - total);
    n := part;
    if n = 0 then
      eof := true
    else
      inc(total, n);
  until eof or (total >= 16);
  NetClose(conn);
  if (not eof) or (total <> 8) then
    gShutOk := false;
end;

var
  rp1, rp2, listener, shlistener, udpsocket, quietsocket: TSocket;
  opt: TNetListenOpts;
  shPort: Integer;
  resolveHost: string;
begin
  { two sockets on one port, both carrying SO_REUSEPORT }
  opt.ReuseAddr := true;
  opt.ReusePort := true;
  rp1 := NetListenTo('127.0.0.1', 0, 8, opt);
  rp2 := NetListenTo('127.0.0.1', NetBoundPort(rp1), 8, opt);
  if (rp1 >= 0) and (rp2 >= 0) and
     (NetBoundPort(rp2) = NetBoundPort(rp1)) then
    Writeln('reuseport: ok')
  else
  begin
    Writeln('reuseport: FAIL');
    halt(1);
  end;
  NetClose(rp1);
  NetClose(rp2);

  { the name path of NetConnect and the pool-thread resolver }
  resolveHost := Wait(NetResolve('localhost'));
  if resolveHost <> '' then
    Writeln('resolve: ', resolveHost)
  else
  begin
    Writeln('resolve: FAIL');
    halt(1);
  end;

  { one listener, two waiters: the accept task serves, the extra waiter
    parks beside it and wakes on the same readiness; a second listener
    carries the half-close scenario }
  opt.ReuseAddr := false;
  opt.ReusePort := false;
  listener := NetListenTo('127.0.0.1', 0, 8, opt);
  gPort := NetBoundPort(listener);
  shlistener := NetListenTo('127.0.0.1', 0, 8, opt);
  shPort := NetBoundPort(shlistener);
  udpsocket := NetUdpBind('', 0);
  gUdpPort := NetBoundPort(udpsocket);
  quietsocket := NetUdpBind('127.0.0.1', 0);
  scope
    spawn Served(listener);
    spawn ExtraWaiter(listener);
    spawn TimedWaiter(quietsocket);
    spawn UdpServed(udpsocket);
    spawn UdpClient;
    spawn ShutServed(shlistener, 'pong!');
    spawn ShutClient(shPort);
    spawn Client(1, '127.0.0.1', 'one');
    spawn Client(2, '127.0.0.1', '2nd');
    spawn Client(3, '127.0.0.1', 'three!');
    spawn Client(4, 'localhost', 'four');
  end;
  NetClose(listener);
  NetClose(shlistener);
  NetClose(udpsocket);
  Writeln('client 1: ', gClients[1]);
  Writeln('client 2: ', gClients[2]);
  Writeln('client 3: ', gClients[3]);
  Writeln('client 4: ', gClients[4]);
  Writeln('served: ', gServed);
  Writeln('extra parks: ', gExtraParks);
  if (gServed <> 4) or (gExtraParks <> 1) then
    halt(1);
  Writeln('udp 1: ', gUdp1);
  Writeln('udp 2: ', gUdp2);
  if gExtraBits then
    Writeln('extra waits: ok')
  else
  begin
    Writeln('extra waits: FAIL');
    halt(1);
  end;
  if gKeepAlive then
    Writeln('keepalive: set')
  else
  begin
    Writeln('keepalive: FAIL');
    halt(1);
  end;
  if gShutOk then
    Writeln('shutwrite: ok')
  else
  begin
    Writeln('shutwrite: FAIL');
    halt(1);
  end;
  if gWaitOk then
    Writeln('wait timeout: ok')
  else
  begin
    Writeln('wait timeout: FAIL');
    halt(1);
  end;
  Writeln('waits abandoned: ', NetWaitsAbandoned);
  if NetWaitsAbandoned <> 0 then
    halt(1);
end.
