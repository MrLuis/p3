{$H+}
{$modeswitch racechecks}
program e1;
uses threading;

type
  TCounter = specialize TAtomic<Integer>;
  TBytes = array of Byte;
  TThing = class
    Count: Integer;
    Hits: shared TCounter;
    Data: isolated TBytes;
    procedure Go: async;
  end;
  TSafe = class(TSharedObject)
    Count: Integer;
    procedure Go: async;
  end;

var
  globalCount: Integer;
  globalHits: shared TCounter;
  wrong: shared Integer;
  alsoWrong: isolated Integer;

procedure Use(b: TBytes): async;
begin
end;

procedure TThing.Go: async;
begin
  scope
    spawn begin
      Hits.Increment;
      Inc(Count);
    end;
  end;
end;

procedure TSafe.Go: async;
begin
  scope
    spawn begin
      Inc(Count);
    end;
  end;
end;

procedure Run(n: Integer; const k: Integer; const o: TThing): async;
var
  sum: Integer;
  buffer, other: isolated TBytes;
  plain: TBytes;
  counter: shared TCounter;

  procedure Helper;
  begin
    Inc(sum);
  end;

begin
  scope
    spawn begin
      Inc(sum);
      Inc(globalCount);
      globalHits.Increment;
      Writeln(n);
      Writeln(k);
      o.Go;
      buffer[0] := 1;
      Helper;
      counter := nil;
      counter.Increment;
    end;
  end;
  plain := buffer;
  var copy := other;
  Use(buffer);
  Use(move buffer);
  plain := move other;
  plain := move plain;
end;

begin
end.
