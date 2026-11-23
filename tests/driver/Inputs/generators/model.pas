program model;
{$modeswitch advancedrecords}
type
  [Serializable(Format.Json, 'x y', 3, -4, True, 1.5)]
  TCustomer = record
    Id: Integer;
    Name: string;
    [Skip] Cache: TObject;
    [Doc('one')] function One(a: Integer; var b: string): Integer;
    procedure Run;
  end;
  [Other] TOther = record X: Integer; end;
  [Serializable] TNode = class(TObject)
  private
    FNext: array of TNode;
  public
    constructor Create;
  end;
  [Serializable] TColor = (Red, Green);
begin
end.
