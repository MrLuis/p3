program ok;
type
  TA = partial record
    X: Integer;
  end;
  TA = partial record
    Y: Integer;
  end;
  TB = partial class(TObject)
    FA: Integer;
  end;
  TB = partial class
    FB: Integer;
  end;
begin
end.
