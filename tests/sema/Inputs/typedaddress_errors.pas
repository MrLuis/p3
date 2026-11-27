{$T+}
program TypedAddressErrors;

type
  PInt = ^integer;
  PByte = ^byte;
  TRec = record a, b: int64; end;
  PRec = ^TRec;

var
  x: integer;
  b: byte;
  pi: PInt;
  pb: PByte;
  pr: PRec;
begin
  pi := @b;
  pb := @x;
  pr := @b;
  if pi = @b then writeln;
  {$push}
  {$T-}
  pi := @b; { ok under $T- }
  {$pop}
  pi := @b; { rejected again after $pop }
  {$typedaddress off}
  pi := @b; { ok under $typedaddress off }
  {$typedaddress on}
  pb := @x; { rejected again after $typedaddress on }
end.
