{ included by d07_tables.pas (R4) }
type
  TTokKind = (tkIdent, tkBegin, tkEnd, tkIf);
  TKeyword = record
    spelling: string[8];
    kind: TTokKind;
  end;
const
  keywords: array[1..3] of TKeyword = (
    (spelling: 'begin'; kind: tkBegin),
    (spelling: 'end';   kind: tkEnd),
    (spelling: 'if';    kind: tkIf));
  kindNames: array[TTokKind] of string[6] = ('ident', 'begin', 'end', 'if');
