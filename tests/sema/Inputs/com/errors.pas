program errors;
type
  IA = interface
    procedure P;
  end;
  TPlain = class(TObject, IA)
    procedure P;
  end;
{$interfaces corba}
  IC = interface
    procedure Q;
  end;
  IFromCom = interface(IA)
  end;
{$interfaces com}
  IFromCorba = interface(IC)
  end;
  TVariant = record
    case k: integer of
      0: (a: IA);
      1: (n: integer);
  end;
  TFile = file of IA;
  TRec = record x: IA; end;

procedure TPlain.P;
begin
end;

function Bad: TRec;
begin
end;

var
  c: IC;
  a: IA;
  o: TObject;
begin
  if c is TObject then;
  o := a as TObject;
  if a is IC then;
end.
