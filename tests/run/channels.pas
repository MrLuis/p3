{ Unit threading (objfpc): TChannel<T> between tasks (Send waits while a
  bounded channel is full, Receive while it is empty, Close ends
  `for await`), and TCancelToken's methods (Create, Cancel, IsCancelled,
  WithTimeout, NewChild, ThrowIfCancelled).
  RUN: %pcc --mode=objfpc %s -o %t
  RUN: %t | diff - %S/channels.expected
  RUN: %pcc --mode=objfpc -O2 %s -o %t.fast
  RUN: %t.fast | diff - %S/channels.expected
}
{$H+}
program channels;

uses SysUtils, threading;

type
  TJobs = specialize TChannel<Integer>;
  TNames = specialize TChannel<string>;

{ a producer, four workers and a bounded channel; a channel of one value
  is the lock around the sum }
function Pipeline(Token: TCancelToken): async Integer;
var
  jobs, lock: TJobs;
  sum: Integer;
begin
  jobs := TJobs.Create(4);
  lock := TJobs.Create(1);
  sum := 0;
  await lock.Send(0);
  scope Token
    spawn begin
      for var j := 1 to 100 do
        await jobs.Send(j);
      jobs.Close;
    end;
    for var w := 1 to 4 do
      spawn begin
        for await var j in jobs do
        begin
          var s := await lock.Receive;
          sum := sum + j;
          await lock.Send(s);
        end;
      end;
  end;
  Result := sum;
  jobs.Free;
  lock.Free;
end;

function Names: async string;
var
  ch: TNames;
  n: string;
begin
  ch := TNames.Create;
  await ch.Send('ann');
  await ch.Send('bob');
  await ch.Send('cy');
  Writeln('queued: ', ch.Count);
  ch.Close;
  Result := '';
  for await n in ch do
    Result := Result + n + ';';
  try
    await ch.Send('dan');
  except
    on E: EChannelClosed do
      Result := Result + ' (' + E.Message + ')';
  end;
  try
    n := await ch.Receive;
  except
    on E: EChannelClosed do
      Result := Result + ' (' + E.Message + ')';
  end;
  ch.Free;
end;

{ an unbounded channel grows }
function Many: async Integer;
var
  ch: TJobs;
  i: Integer;
begin
  ch := TJobs.Create;
  for i := 1 to 1000 do
    await ch.Send(i);
  ch.Close;
  Result := 0;
  for await var v in ch do
    Result := Result + v;
  ch.Free;
end;

function Forever(ms: Integer): async Integer;
begin
  Sleep(ms);
  Result := ms;
end;

function TimesOut(Token: TCancelToken): async Integer;
begin
  scope Token.WithTimeout(30)
    spawn Forever(5000);
    spawn Forever(6000);
  end;
  Result := 1;
end;

function Checks(Token: TCancelToken): async Integer;
begin
  Result := 0;
  while true do
  begin
    Token.ThrowIfCancelled;
    Inc(Result);
    Sleep(1);
  end;
end;

var
  k, child: TCancelToken;
  t: async Integer;
  got: string;
begin
  Writeln('pipeline: ', Wait(Pipeline(nil)));
  got := Wait(Names);
  Writeln('names: ', got);
  Writeln('many: ', Wait(Many));
  k := TCancelToken.Create;
  try
    Writeln(Wait(TimesOut(k)));
  except
    on E: EOperationCancelled do
      Writeln('timeout: ', E.Message, ' ', k.IsCancelled);
  end;
  child := k.NewChild;
  Writeln(child.IsCancelled);
  k.Cancel;
  Writeln(k.IsCancelled, ' ', child.IsCancelled);
  k := TCancelToken.Create;
  t := Checks(k);
  Sleep(20);
  k.Cancel;
  try
    Wait(t);
  except
    on E: EOperationCancelled do
      Writeln('checked: ', E.ClassName);
  end;
  k := nil;
  child := nil;
end.
