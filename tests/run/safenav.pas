{ Safe navigation (objfpc; item 2 of the proposals, without the nullsafe switch):
  x?.m is m of x, or the default value of m's type where x is nil (a call
  as a statement is skipped); a ?. b?. c stops at the first nil; a ?? b is
  a, or b where a is nil (or where a's ?. chain stopped).
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t | diff - %S/safenav.expected
  RUN: %p3 --mode=objfpc -O2 %s -o %t.fast
  RUN: %t.fast | diff - %S/safenav.expected
}
{$H+}
program safenav;
type
  TCustomer = class
    Name: string;
    Age: Integer;
    Friend: TCustomer?;
    constructor Create(const AName: string; AAge: Integer);
    procedure Greet;
    function Upper: string;
  end;
  IThing = interface
    function Size: Integer;
  end;
  TThing = class(TInterfacedObject, IThing)
    function Size: Integer;
  end;

constructor TCustomer.Create(const AName: string; AAge: Integer);
begin
  Name := AName;
  Age := AAge;
  Friend := nil;
end;

procedure TCustomer.Greet;
begin
  Writeln('hello ', Name);
end;

function TCustomer.Upper: string;
begin
  Result := Name + '!';
end;

function TThing.Size: Integer;
begin
  Result := 42;
end;

procedure Test;
var
  a, b: TCustomer?;
  t: IThing?;
  s: string;
begin
  a := TCustomer.Create('ann', 30);
  b := nil;
  Writeln(a?.Name ?? 'unknown', ' ', b?.Name ?? 'unknown');
  Writeln(a?.Age, ' ', b?.Age, ' ', b?.Age ?? -1);
  a?.Greet;
  b?.Greet;
  Writeln(a?.Upper, '|', b?.Upper, '|');
  a.Friend := TCustomer.Create('bob', 40);
  Writeln(a?.Friend?.Name ?? 'none', ' ', b?.Friend?.Name ?? 'none');
  Writeln((a.Friend ?? a).Name, ' ', (b ?? a).Name);
  s := a?.Friend?.Friend?.Name ?? 'nobody';
  Writeln(s);
  t := TThing.Create;
  Writeln(t?.Size ?? 0);
  t := nil;
  Writeln(t?.Size ?? 0);
  a.Friend.Free;
  a.Free;
end;

begin
  Test;
end.
