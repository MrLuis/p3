{ netasync: NetReadSome with a Max of 1 MB (it gives what one recv got, up
  to 64 KB) moves 300000 bytes whole; small Max values still work.
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t | diff - %S/netasync-bigread.expected
  RUN: %p3 --mode=objfpc -O2 %s -o %t.O2
  RUN: %t.O2 | diff - %S/netasync-bigread.expected
}
{$mode objfpc}{$H+}
program netasync_bigread;
uses netasync, sysutils;

const
  Total = 300000;

var
  gPort: Integer;
  gSum, gCount, gMax, gSmall: Int64;

function Sender(listener: TSocket): async;
var
  conn: TSocket;
  data: TNetBytes;
  i: Integer;
begin
  SetLength(data, Total);
  for i := 0 to Total - 1 do
    data[i] := Byte((i * 7 + i div 256) and 255);
  conn := await NetAccept(listener);
  var sent := await NetSendAll(conn, data);
  NetClose(conn);
end;

function Receiver: async;
var
  conn: TSocket;
  chunk: TNetBytes;
  got, i: Integer;
begin
  conn := await NetConnect('127.0.0.1', gPort);
  got := 0;
  { the first read asks for 5 bytes only }
  chunk := await NetReadSome(conn, 5);
  gSmall := Length(chunk);
  for i := 0 to High(chunk) do
    gSum := gSum + chunk[i];
  got := Length(chunk);
  while got < Total do
  begin
    chunk := await NetReadSome(conn, 1 shl 20);
    if Length(chunk) = 0 then
      break;
    if Length(chunk) > gMax then
      gMax := Length(chunk);
    for i := 0 to High(chunk) do
      gSum := gSum + chunk[i];
    got := got + Length(chunk);
  end;
  gCount := got;
  NetClose(conn);
end;

var
  listener: TSocket;
  i: Integer;
  want: Int64;
begin
  listener := NetListen(0, 16, true);
  gPort := NetBoundPort(listener);
  scope
    spawn Sender(listener);
    spawn Receiver;
  end;
  NetClose(listener);
  want := 0;
  for i := 0 to Total - 1 do
    want := want + Byte((i * 7 + i div 256) and 255);
  Writeln('first read: ', gSmall);
  Writeln('bytes: ', gCount, ' checksum ok: ', gSum = want);
  Writeln('no read above 64 KB: ', gMax <= 65536);
end.
