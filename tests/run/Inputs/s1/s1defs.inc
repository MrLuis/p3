{ included by s1.pas, which is in the parent directory's parent }
type
  TOp = (opAdd, opSub, opMul);
  TBinFn = function(a, b: longint): longint;
  TEntry = record
    name: string[5];
    op: TOp;
    code: array[1..2] of byte;
  end;
{$I s1more.inc}
