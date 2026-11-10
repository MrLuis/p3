{ Exceptions: try..except and try..finally entered often, raised rarely. }
program exceptions;
uses sysutils;

type
  ELimit = class(Exception);

function Check(n: longint): longint;
begin
  if n mod 5000 = 0 then
    raise ELimit.Create('limit');
  Check := n and 7;
end;

function Guarded(n: longint): longint;
var acc: longint;
begin
  acc := 0;
  try
    acc := acc + Check(n);
    acc := acc + Check(n + 1);
  except
    on E: ELimit do
      acc := -1;
  end;
  Guarded := acc;
end;

function Cleanup(n: longint): longint;
var acc, i: longint;
begin
  acc := 0;
  try
    for i := 1 to 4 do
      acc := acc + (n xor i);
  finally
    acc := acc and $FF;
  end;
  Cleanup := acc;
end;

var i, sum, caught: longint;
begin
  sum := 0;
  caught := 0;
  for i := 1 to 20000000 do
  begin
    if Guarded(i) < 0 then
      caught := caught + 1
    else
      sum := (sum + Cleanup(i)) and $FFFFFF;
  end;
  writeln(sum, ' ', caught);
end.
