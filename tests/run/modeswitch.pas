{ The MODESWITCH directive, as in Free Pascal: here in tp mode, result
  (Result names a function's result), out (out parameters) and
  defaultparameters (parameters' default values) turned on; names that tp
  mode already has (cvar, initfinal, hintdirective) and the + / ON forms
  are accepted. Output is identical to Free Pascal 3.2.2's (fpc -Mtp).
  (Free Pascal takes mode switches only at the start of a file; p3 also
  later.)
  RUN: %p3 --mode=tp %s -o %t
  RUN: %t | diff - %S/modeswitch.expected
}
program modeswitch;
{$modeswitch result}
{$modeswitch out+}
{$modeswitch defaultparameters on}
{$modeswitch cvar}
{$modeswitch initfinal}
{$modeswitch hintdirective}

function Twice(x: integer): integer;
begin
  Result := x * 2;
end;

procedure Fill(out a, b: integer);
begin
  a := 1;
  b := 2;
end;

function Add(x: integer; y: integer = 10): integer;
begin
  Add := x + y;
end;

var
  p, q: integer;
begin
  Fill(p, q);
  writeln(Twice(21), ' ', p, ' ', q, ' ', Add(1), ' ', Add(1, 2));
end.
