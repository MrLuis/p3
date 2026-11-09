unit money;
{$modeswitch advancedrecords}

interface

type
  TMoney = record
  private
    FCents: integer;
  public
    constructor Create(units, cents: integer);
    function Str: string;
    procedure Add(cents: integer);
    class function Zero: TMoney; static;
    class operator +(const a, b: TMoney): TMoney;
    class operator =(const a, b: TMoney): boolean;
    class operator :=(cents: integer): TMoney;
    property Cents: integer read FCents;
  end;

implementation

function Two(n: integer): string;
begin
  Two := chr(ord('0') + n div 10) + chr(ord('0') + n mod 10);
end;

constructor TMoney.Create(units, cents: integer);
begin
  FCents := units * 100 + cents;
end;

function TMoney.Str: string;
begin
  Str := chr(ord('0') + FCents div 100) + '.' + Two(FCents mod 100);
end;

procedure TMoney.Add(cents: integer);
begin
  FCents := FCents + cents;
end;

class function TMoney.Zero: TMoney;
begin
  Result.FCents := 0;
end;

class operator TMoney.+(const a, b: TMoney): TMoney;
begin
  Result.FCents := a.FCents + b.FCents;
end;

class operator TMoney.=(const a, b: TMoney): boolean;
begin
  Result := a.FCents = b.FCents;
end;

class operator TMoney.:=(cents: integer): TMoney;
begin
  Result.FCents := cents;
end;

end.
