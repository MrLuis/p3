{ Attributes: [Name] and [Name(args)] before types, fields, methods,
  properties and routines. The compiler gives them no meaning (a source
  generator does); a program with attributes behaves as without them.
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t | diff - %S/attributes.expected
}
program attributes;
{$modeswitch advancedrecords}

type
  [Serializable(1, 'json')]
  TCustomer = record
    [Key] Id: Integer;
    Name: string;
    [Skip, Other] Cache, Other2: Pointer;
    [Doc('says one')] function One: Integer;
  end;

  [Entity] [Table('bases')]
  TBase = class
    [Column('a')] FA: Integer;
    [Hidden]
    procedure Run; virtual;
    [Prop] property A: Integer read FA;
  end;

  TPlain = Integer;

const
  Level = 3;

type
  [Levelled(Level * 2 + 1, True)]
  TLeveled = class
  end;

[Fast]
function Twice(x: Integer): Integer;
begin
  Result := x * 2;
end;

function TCustomer.One: Integer;
begin
  Result := 1;
end;

procedure TBase.Run;
begin
  FA := FA + 1;
end;

var
  c: TCustomer;
  b: TBase;
begin
  c.Id := 7;
  b := TBase.Create;
  b.Run;
  b.Run;
  writeln(Twice(21), ' ', c.One, ' ', c.Id, ' ', b.A);
end.
