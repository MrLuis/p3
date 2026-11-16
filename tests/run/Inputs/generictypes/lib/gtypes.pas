unit gtypes;
{$H+}
interface

type
  generic TBag<T> = class
  private
    FItems: array of T;
  public
    procedure Add(const Item: T);
    function Count: Integer;
    function Item(i: Integer): T;
  end;

  generic TPair<A, B> = record
    First: A;
    Second: B;
  end;

  TNames = specialize TBag<string>;

function MakeNames: TNames;

implementation

procedure TBag.Add(const Item: T);
begin
  SetLength(FItems, Length(FItems) + 1);
  FItems[High(FItems)] := Item;
end;

function TBag.Count: Integer;
begin
  Result := Length(FItems);
end;

function TBag.Item(i: Integer): T;
begin
  Result := FItems[i];
end;

function MakeNames: TNames;
begin
  Result := TNames.Create;
  Result.Add('ann');
  Result.Add('bo');
end;

end.
