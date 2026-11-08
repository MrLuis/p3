program include;
{$define MAIN}
{$ifdef MAIN}
{$I cond.inc}
{$endif}
{$ifdef NOPE}
{$I missing.inc}
{$endif}
begin
  {$ifdef FROM_INC} a {$endif}
  {$ifdef MAIN} b {$else} c {$endif}
end.
