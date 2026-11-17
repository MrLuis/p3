{ A unit with external routines: their libraries are linked with the
  programs using it. }
unit clib;

interface

const
  libc = 'c';

function c_labs(x: int64): int64; cdecl; external libc name 'labs';
function missing1(x: longint): longint; cdecl; external 'p3_no_such_library';

implementation

function missing2: longint; cdecl; external 'libp3_no_such.so.1' name 'missing2';

end.
