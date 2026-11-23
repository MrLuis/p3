program errors;
type
  TPlain = record
    X: Integer;
  end;
  TPlain = partial record
    Y: Integer;
  end;
  TR = partial record
    X: Integer;
  end;
  TR = partial class
    FA: Integer;
  end;
  TP = partial record
    X: Integer;
  end;
  TP = partial packed record
    Y: Integer;
  end;
  TC = partial class(TObject)
  end;
  TC = partial class(TObject)
  end;
  TF = partial class;
begin
end.
