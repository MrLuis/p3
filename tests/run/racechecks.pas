{ Compile-time data-race checks (objfpc): under the racechecks modeswitch a
  spawned task may use only shared variables of the routines around it
  (of thread-safe types: TAtomic<T>, TLocked<T>, TChannel<T>, classes
  derived from TSharedObject); an isolated variable belongs to one task
  and is handed over with move (left empty).
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t | diff - %S/racechecks.expected
  RUN: %p3 --mode=objfpc -O2 %s -o %t.fast
  RUN: %t.fast | diff - %S/racechecks.expected
}
{$H+}
{$modeswitch racechecks}
program racechecks;

uses SysUtils, threading;

type
  TCounter = specialize TAtomic<Integer>;
  TStrings = array of string;
  TNames = specialize TLocked<TStrings>;
  TBytes = array of Byte;
  TJobs = specialize TChannel<TBytes>;

  { thread-safe: its spawned tasks may use Self }
  TTally = class(TSharedObject)
    Hits: shared TCounter;
    constructor Create;
    destructor Destroy; override;
    function Run(n: Integer): async Integer;
  end;

constructor TTally.Create;
begin
  Hits := TCounter.Create(0);
end;

destructor TTally.Destroy;
begin
  Hits.Free;
  inherited;
end;

function TTally.Run(n: Integer): async Integer;
begin
  scope
    for var i := 1 to n do
      spawn begin
        Hits.Add(10);
      end;
  end;
  Result := Hits.Load;
end;

function Total(buf: TBytes): async Integer;
var
  b: Byte;
begin
  Result := 0;
  for b in buf do
    Result := Result + b;
end;

{ on the pool's threads at once }
function Bump(c: TCounter; times: Integer): async;
var
  i: Integer;
begin
  for i := 1 to times do
    c.Increment;
end;

function Run(n: Integer): async Integer;
var
  counter: shared TCounter;
  names: shared TNames;
  buffer: isolated TBytes;
  jobs: shared TJobs;
  t: async Integer;
begin
  counter := TCounter.Create(0);
  names := TNames.Create(nil);
  jobs := TJobs.Create;
  scope
    for var i := 1 to n do
      spawn begin
        counter.Increment;
        names.Use(procedure(var a: TStrings) =>
                    begin
                      SetLength(a, Length(a) + 1);
                      a[High(a)] := 'task';
                    end);
      end;
  end;
  Writeln('counted: ', counter.Load, ', names: ', Length(names.Load));
  Writeln('exchange: ', counter.Exchange(100), ' ', counter.Load);
  Writeln('cas: ', counter.CompareExchange(100, 7), ' ',
          counter.CompareExchange(100, 8), ' ', counter.Load);
  Writeln('add: ', counter.Add(3), ', dec: ', counter.Decrement);

  SetLength(buffer, 3);
  buffer[0] := 1;
  buffer[1] := 2;
  buffer[2] := 3;
  t := Total(move buffer);
  Writeln('moved: ', Length(buffer));
  Writeln('total: ', await t);
  SetLength(buffer, 2);
  buffer[1] := 40;
  await jobs.Send(move buffer);
  jobs.Close;
  for await var b in jobs do
    Writeln('received ', Length(b), ' ', b[1]);
  Result := counter.Load;
  counter.Free;
  names.Free;
  jobs.Free;
end;

var
  c: TCounter;
  ts: array[0..3] of async;
  tally: TTally;
  i: Integer;
begin
  i := Wait(Run(5));
  Writeln('run: ', i);
  c := TCounter.Create(0);
  for i := 0 to 3 do
    ts[i] := Pool.Run(Bump(c, 10000));
  for i := 0 to 3 do
    Wait(ts[i]);
  Writeln('pool: ', c.Load);
  c.Free;
  tally := TTally.Create;
  Writeln('tally: ', Wait(tally.Run(4)));
  tally.Free;
end.
