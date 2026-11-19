program devirt;

type
  TBase = class
    procedure Unchanged; virtual;
    procedure Changed; virtual;
  end;

  TChild = class(TBase)
    procedure Changed; override;
  end;

  TLeaf = class(TChild)
  end;

procedure TBase.Unchanged;
begin
end;

procedure TBase.Changed;
begin
end;

procedure TChild.Changed;
begin
end;

var
  B: TBase;
  L: TLeaf;

begin
  B := TBase.Create;
  B.Unchanged;
  B.Changed;
  L := TLeaf.Create;
  L.Unchanged;
  L.Changed;
  B.Destroy;
  L.Destroy;
  TChild.Create.Changed
end.
