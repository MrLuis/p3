{ Checked conversions (objfpc): in a checked block or expression, an
  integer value converted to a narrower integer type (assignment,
  argument) is checked whatever $R says, raising EIntOverflow; a typecast
  still truncates, a subrange follows $R, and unchecked wraps.
  RUN: %p3 --mode=objfpc %s -o %t
  RUN: %t | diff - %S/checkedconv.expected
  RUN: %p3 --mode=objfpc -O2 %s -o %t.fast
  RUN: %t.fast | diff - %S/checkedconv.expected
  RUN: %p3 --mode=objfpc -g %s -o %t.dbg
  RUN: %t.dbg | diff - %S/checkedconv.expected
}
program checkedconv;
{$R-}
type
  TSmall = 1..10;

procedure Take(b: Byte);
begin
  Writeln('took ', b);
end;

var
  b: Byte;
  w: Word;
  i: Integer;
  s: TSmall;
  q: QWord;
begin
  i := 300;
  b := i;                { $R-: truncates }
  Writeln(b);
  checked
    try
      b := i;            { checked: raises }
    except
      on E: EIntOverflow do Writeln('assign: ', E.Message);
    end;
    try
      Take(i);
    except
      on E: EIntOverflow do Writeln('argument: ', E.Message);
    end;
    b := Byte(i);        { a typecast: truncates }
    Writeln(b);
    i := 200;
    b := i;              { fits }
    Writeln(b);
    i := 50;
    s := i;              { a subrange: $R decides }
    Writeln(s);
    q := QWord(-1);
    try
      w := q;
    except
      on E: EIntOverflow do Writeln('qword: ', E.Message);
    end;
    unchecked
      w := 70000;
      Writeln(w);
    end;
  end;
  try
    i := 1000;
    b := checked(i);
  except
    on E: EIntOverflow do Writeln('expression: ', E.Message);
  end;
end.
