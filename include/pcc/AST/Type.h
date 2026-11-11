//===- Type.h - Semantic types ------------------------------------*- C++ -*-===//
//
// Types as Sema understands them (as opposed to TypeSpec, which is how a type
// is *written*). Types live in the ASTContext arena. ISO Pascal uses name
// equivalence: every `array`/`record`/enumeration written in the source is a
// distinct type, so type identity is pointer identity.
//
//   simple:     integer types, real, boolean, char, enumerations, subranges
//               (Turbo/FPC adds int64, cardinal, byte, word, shortint and
//               smallint; longint is integer)
//   structured: arrays, records (with variant parts)
//   internal:   String (a string literal of length N), Error
//
// Pointers/sets (M9) and files (M10) come later.
//
//===----------------------------------------------------------------------===//
#ifndef PCC_AST_TYPE_H
#define PCC_AST_TYPE_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Casting.h"

#include <cstdint>
#include <string>

namespace pcc {

class ConstDecl;
class Decl;
class FieldDecl;
class RoutineDecl;
class VarDecl;
struct FieldList;

class Type {
public:
  enum TypeKind {
    TK_Error,    ///< result of a failed check; suppresses follow-up errors
    TK_Integer,
    TK_Real,
    TK_Boolean,
    TK_Char,
    TK_Enum,
    TK_Subrange,
    TK_String,   ///< a string literal of length != 1
    TK_Array,
    TK_Record,
    TK_Pointer,  ///< ^T                                         (M9)
    TK_Nil,      ///< the type of `nil`, compatible with every pointer
    TK_Set,      ///< set of T (T's values must lie in 0..255)   (M9)
    TK_File,     ///< file of T, text                            (M10)
    TK_Routine,  ///< a procedural/functional parameter's type   (M10)
    TK_ShortString, ///< Turbo/FPC string[N]: length byte + N chars (M12)
    TK_AnsiString, ///< ansistring: a reference-counted pointer to chars
    TK_ArrayOfConst, ///< a parameter's `array of const`: TVarRec elements
    TK_OpenArray, ///< a parameter's `array of T`
    TK_DynArray, ///< a dynamic array (Turbo/FPC): a counted reference
    TK_Class,    ///< class ... end (objfpc): a reference to an object
    TK_ClassRef, ///< class of T (objfpc): a reference to a class (its VMT)
    TK_Interface, ///< interface ... end (objfpc): {object, method table}
    TK_Untyped,  ///< an untyped parameter's (Turbo/FPC `var x`, `const x`)
  };

private:
  TypeKind Kind;
  llvm::StringRef DeclName; ///< first name given in a type declaration

protected:
  explicit Type(TypeKind K) : Kind(K) {}

public:
  TypeKind getKind() const { return Kind; }

  bool isError() const { return Kind == TK_Error; }
  bool isInteger() const { return Kind == TK_Integer; }
  bool isReal() const { return Kind == TK_Real; }
  bool isBoolean() const { return Kind == TK_Boolean; }
  bool isChar() const { return Kind == TK_Char; }
  bool isEnum() const { return Kind == TK_Enum; }
  bool isSubrange() const { return Kind == TK_Subrange; }
  bool isStringLiteral() const { return Kind == TK_String; }
  bool isArray() const { return Kind == TK_Array; }
  bool isRecord() const { return Kind == TK_Record; }
  bool isPointer() const { return Kind == TK_Pointer; }
  bool isNil() const { return Kind == TK_Nil; }
  bool isUntyped() const { return Kind == TK_Untyped; }
  bool isSet() const { return Kind == TK_Set; }
  bool isFile() const { return Kind == TK_File; }
  bool isShortString() const { return Kind == TK_ShortString; }
  bool isAnsiString() const { return Kind == TK_AnsiString; }
  bool isArrayOfConst() const { return Kind == TK_ArrayOfConst; }
  bool isOpenArray() const { return Kind == TK_OpenArray; }
  bool isDynArray() const { return Kind == TK_DynArray; }
  bool isClass() const { return Kind == TK_Class; }
  bool isClassRef() const { return Kind == TK_ClassRef; }
  bool isInterface() const { return Kind == TK_Interface; }
  /// Anything that converts to a short string: short strings, ansistrings,
  /// string types, and chars (Turbo/FPC).
  bool isStringish() const;
  bool isTextFile() const;

  /// ISO 7185 6.4.2.1: integer, boolean, char, enumerations and subranges.
  bool isOrdinal() const {
    return Kind == TK_Integer || Kind == TK_Boolean || Kind == TK_Char ||
           Kind == TK_Enum || Kind == TK_Subrange;
  }
  bool isSimple() const { return isOrdinal() || isReal(); }
  /// Types a function may return (ISO 6.6.2: simple or pointer; classes).
  bool isValidResultType() const {
    return isSimple() || isPointer() || isClass() || isClassRef() ||
           isInterface() || isAnsiString() || isDynArray();
  }
  bool isStructured() const { return isArray() || isRecord() || isShortString(); }

  /// Integer-like after stripping subranges (an integer type or a subrange
  /// of one).
  bool isIntegerLike() const;
  /// Integer-like with a signed host type (integer, int64, shortint,
  /// smallint). Unsigned hosts are byte, word and cardinal.
  bool isSignedIntegerLike() const;
  /// integer, real, or a subrange of integer.
  bool isNumeric() const { return isIntegerLike() || isReal(); }

  /// Host type of a subrange (recursively); the type itself otherwise.
  Type *getBase();

  /// A string type: a string literal, or `packed array [1..n] of char`.
  bool isStringType() const;
  /// Number of chars of a string type.
  uint64_t getStringLength() const;

  llvm::StringRef getDeclName() const { return DeclName; }
  void setDeclName(llvm::StringRef N) {
    if (DeclName.empty())
      DeclName = N;
  }

  /// Name for diagnostics: the declared name if any, else a description
  /// such as "array [1..10] of integer".
  std::string getName() const;
};

/// The predeclared simple types and the error type.
class BuiltinType : public Type {
public:
  explicit BuiltinType(TypeKind K) : Type(K) {}
  static bool classof(const Type *T) {
    switch (T->getKind()) {
    case TK_Error: case TK_Integer: case TK_Real: case TK_Boolean: case TK_Char:
    case TK_Nil: case TK_Untyped:
      return true;
    default:
      return false;
    }
  }
};

/// A predeclared integer type: integer (= longint, 32-bit signed), and in
/// --mode=tp also int64, cardinal, byte, word, shortint, smallint.
class IntegerType final : public BuiltinType {
  unsigned Bits;
  bool Signed;
  llvm::StringRef Name;

public:
  IntegerType(unsigned Bits, bool Signed, llvm::StringRef Name)
      : BuiltinType(TK_Integer), Bits(Bits), Signed(Signed), Name(Name) {}
  unsigned getBits() const { return Bits; }
  bool isSigned() const { return Signed; }
  llvm::StringRef getIntegerName() const { return Name; }
  static bool classof(const Type *T) { return T->getKind() == TK_Integer; }
};

/// (red, green, blue) -- values 0..N-1.
class EnumType final : public Type {
  llvm::ArrayRef<ConstDecl *> Values;

public:
  EnumType() : Type(TK_Enum) {}
  llvm::ArrayRef<ConstDecl *> getValues() const { return Values; }
  void setValues(llvm::ArrayRef<ConstDecl *> V) { Values = V; }
  static bool classof(const Type *T) { return T->getKind() == TK_Enum; }
};

/// Lo..Hi of a host ordinal type.
class SubrangeType final : public Type {
  Type *Host;
  int64_t Lo, Hi;

public:
  SubrangeType(Type *Host, int64_t Lo, int64_t Hi)
      : Type(TK_Subrange), Host(Host), Lo(Lo), Hi(Hi) {}
  Type *getHost() const { return Host; }
  int64_t getLo() const { return Lo; }
  int64_t getHi() const { return Hi; }
  static bool classof(const Type *T) { return T->getKind() == TK_Subrange; }
};

/// A string literal of a given length (> 1); ISO treats it as a value of
/// `packed array [1..N] of char`.
class StringLiteralType final : public Type {
  uint64_t Length;

public:
  explicit StringLiteralType(uint64_t Length)
      : Type(TK_String), Length(Length) {}
  uint64_t getLength() const { return Length; }
  static bool classof(const Type *T) { return T->getKind() == TK_String; }
};

/// [packed] array [Index] of Element. Multi-dimensional arrays are nested:
/// array [A, B] of T == array [A] of array [B] of T.
class ArrayType final : public Type {
  Type *Index;
  Type *Element;
  bool Packed;
  bool BitPacked = false; ///< bitpacked (Free Pascal)

public:
  ArrayType(Type *Index, Type *Element, bool Packed)
      : Type(TK_Array), Index(Index), Element(Element), Packed(Packed) {}
  Type *getIndexType() const { return Index; }
  Type *getElementType() const { return Element; }
  bool isPacked() const { return Packed; }
  bool isBitPacked() const { return BitPacked; }
  void setBitPacked() { BitPacked = true; }
  /// A bitpacked array of ordinals: its elements take bitPackedWidth bits
  /// each, without addresses.
  bool hasBitElements() const { return BitPacked && Element->isOrdinal(); }
  static bool classof(const Type *T) { return T->getKind() == TK_Array; }
};

/// [packed] record ... end. The layout follows the FieldList tree (fixed
/// part, then an optional variant part whose variants overlay each other).
class ClassType;

class RecordType final : public Type {
  FieldList *Fields;
  llvm::ArrayRef<FieldDecl *> AllFields; ///< flattened, incl. variant fields
  bool Packed;
  bool BitPacked = false; ///< bitpacked (Free Pascal): ordinal fields take
                          ///< bitPackedWidth bits, without addresses
  unsigned PackRecords = 0; ///< {$packrecords N} where declared
  /// An advanced record's (objfpc) member table: a ClassType that is not a
  /// type of values, holding the fields, methods, operators and properties
  /// (see ClassType::getRecord). Null for a plain record.
  ClassType *Members = nullptr;

public:
  RecordType(FieldList *Fields, llvm::ArrayRef<FieldDecl *> AllFields,
             bool Packed)
      : Type(TK_Record), Fields(Fields), AllFields(AllFields), Packed(Packed) {}
  FieldList *getFieldList() const { return Fields; }
  llvm::ArrayRef<FieldDecl *> getAllFields() const { return AllFields; }
  FieldDecl *findField(llvm::StringRef Name) const;
  bool isPacked() const { return Packed; }
  bool isBitPacked() const { return BitPacked; }
  void setBitPacked() { BitPacked = true; }
  /// The largest alignment of its fields: 1 for a packed record, N under
  /// {$packrecords N} (as in Free Pascal); 0: the natural (C) layout.
  unsigned getFieldAlignment() const { return Packed ? 1 : PackRecords; }
  void setPackRecords(unsigned N) { PackRecords = N; }
  ClassType *getMemberTable() const { return Members; }
  void setMemberTable(ClassType *C) { Members = C; }
  static bool classof(const Type *T) { return T->getKind() == TK_Record; }
};

/// The bits an ordinal type T takes in a bitpacked record or array, as in
/// Free Pascal: an integer type's size, 1 for boolean, 8 for char, enough
/// for the largest value of an enumeration or a subrange (two's complement
/// for a subrange with negative values).
unsigned bitPackedWidth(Type *T);
/// Are values of ordinal type T sign-extended from bitPackedWidth bits?
bool bitPackedSigned(Type *T);

/// ^T. The pointee may be declared later in the same type section, so it is
/// filled in once known.
class PointerType final : public Type {
  Type *Pointee;
  bool Untyped = false; ///< Turbo/FPC `pointer`

public:
  explicit PointerType(Type *Pointee, bool Untyped = false)
      : Type(TK_Pointer), Pointee(Pointee), Untyped(Untyped) {}
  bool isUntyped() const { return Untyped; }
  Type *getPointee() const { return Pointee; }
  void setPointee(Type *T) { Pointee = T; }
  static bool classof(const Type *T) { return T->getKind() == TK_Pointer; }
};

/// set of Element. Element is null for the empty set `[]`, which is
/// compatible with every set type.
class SetType final : public Type {
  Type *Element;

public:
  explicit SetType(Type *Element) : Type(TK_Set), Element(Element) {}
  Type *getElementType() const { return Element; }
  bool isEmptySet() const { return Element == nullptr; }
  static bool classof(const Type *T) { return T->getKind() == TK_Set; }
};

/// file of Element; `text` is the predeclared file of char with lines.
class FileType final : public Type {
  Type *Element;
  bool IsText;

public:
  FileType(Type *Element, bool IsText)
      : Type(TK_File), Element(Element), IsText(IsText) {}
  Type *getElementType() const { return Element; }
  bool isText() const { return IsText; }
  static bool classof(const Type *T) { return T->getKind() == TK_File; }
};

/// string[MaxLen] (Turbo/FPC); `string` is string[255].
class ShortStringType final : public Type {
  unsigned MaxLen;

public:
  explicit ShortStringType(unsigned MaxLen)
      : Type(TK_ShortString), MaxLen(MaxLen) {}
  unsigned getMaxLength() const { return MaxLen; }
  static bool classof(const Type *T) { return T->getKind() == TK_ShortString; }
};

/// ansistring: reference counted, any length; `string` under {$H+}.
class AnsiStringType final : public Type {
public:
  AnsiStringType() : Type(TK_AnsiString) {}
  static bool classof(const Type *T) { return T->getKind() == TK_AnsiString; }
};

/// `array of T` as a variable's type (Turbo/FPC): indexed from 0, its length
/// set at run time; a variable holds a reference counted one.
class DynArrayType final : public Type {
  Type *Element;

public:
  explicit DynArrayType(Type *Element) : Type(TK_DynArray), Element(Element) {}
  Type *getElementType() const { return Element; }
  static bool classof(const Type *T) { return T->getKind() == TK_DynArray; }
};

/// A parameter's `array of T` (Turbo/FPC): passed as a {data, count} pair.
class OpenArrayType final : public Type {
  Type *Element;

public:
  explicit OpenArrayType(Type *Element) : Type(TK_OpenArray), Element(Element) {}
  Type *getElementType() const { return Element; }
  static bool classof(const Type *T) { return T->getKind() == TK_OpenArray; }
};

/// A parameter's `array of const` (Turbo/FPC): a count and TVarRec elements.
class ArrayOfConstType final : public Type {
  Type *Element;

public:
  explicit ArrayOfConstType(Type *Element)
      : Type(TK_ArrayOfConst), Element(Element) {}
  Type *getElementType() const { return Element; }
  static bool classof(const Type *T) { return T->getKind() == TK_ArrayOfConst; }
};

/// The type of a procedural (Result == null) or functional parameter.
class RoutineType final : public Type {
  llvm::ArrayRef<VarDecl *> Params;
  Type *Result;
  bool CDecl; ///< 'cdecl' (see CallConv)
  bool OfObject; ///< `of object`: a method and its object

public:
  RoutineType(llvm::ArrayRef<VarDecl *> Params, Type *Result,
              bool CDecl = false, bool OfObject = false)
      : Type(TK_Routine), Params(Params), Result(Result), CDecl(CDecl),
        OfObject(OfObject) {}
  bool isOfObject() const { return OfObject; }
  llvm::ArrayRef<VarDecl *> getParams() const { return Params; }
  Type *getResult() const { return Result; }
  bool isCDecl() const { return CDecl; }
  bool isFunction() const { return Result != nullptr; }
  static bool classof(const Type *T) { return T->getKind() == TK_Routine; }
};

/// class [(Parent)] ... end (objfpc). A value of a class type is a pointer
/// to an object: a block on the heap that starts with a pointer to its
/// class's VMT (virtual method table), followed by the fields of its
/// ancestors and its own. Forward declarations (`TFoo = class;`) create the
/// type undefined; the full declaration fills it in.
class InterfaceType;

/// An interface a class supports (listed by it or an ancestor): the methods
/// implementing the interface's methods, by slot.
struct InterfaceImpl {
  InterfaceType *Intf;
  llvm::ArrayRef<RoutineDecl *> Methods;
};

class ClassType final : public Type {
  ClassType *Parent = nullptr;          ///< null only for TObject
  llvm::ArrayRef<InterfaceImpl> Interfaces;
  llvm::ArrayRef<Decl *> Members;       ///< own members, in source order
  llvm::ArrayRef<FieldDecl *> AllFields; ///< ancestors' fields, then own
  llvm::ArrayRef<RoutineDecl *> VTable; ///< the virtual methods, by slot
  const void *Module = nullptr; ///< the program or unit declaring it
  bool Defined = false;
  RecordType *Record = nullptr; ///< see getRecord
  /// The class (or advanced record's member table) whose type section
  /// declares this one (objfpc nested types); null at program or unit level.
  ClassType *Outer = nullptr;
  /// A type helper's member table: the type it extends (Self's type).
  Type *HelperFor = nullptr;
  bool RecordHelper = false; ///< a record helper (not a type helper)

public:
  ClassType() : Type(TK_Class) {}
  /// The advanced record whose member table this is, or null for a class.
  /// A member table has no parent, VMT or instances: its methods' Self is
  /// the record (by reference), and its class methods and operators have
  /// no Self.
  RecordType *getRecord() const { return Record; }
  void setRecord(RecordType *R) { Record = R; }
  ClassType *getOuter() const { return Outer; }
  bool isHelper() const { return HelperFor != nullptr; }
  Type *getHelperFor() const { return HelperFor; }
  void setHelperFor(Type *T) { HelperFor = T; }
  bool isRecordHelper() const { return RecordHelper; }
  void setRecordHelper() { RecordHelper = true; }
  /// "record helper" or "type helper", for diagnostics.
  llvm::StringRef helperKindName() const {
    return RecordHelper ? "record helper" : "type helper";
  }
  /// Is Self a reference to the value (an advanced record's, a helper's)?
  bool hasSelfByReference() const { return Record || HelperFor; }
  void setOuter(ClassType *C) { Outer = C; }
  ClassType *getParent() const { return Parent; }
  llvm::ArrayRef<Decl *> getMembers() const { return Members; }
  llvm::ArrayRef<FieldDecl *> getAllFields() const { return AllFields; }
  llvm::ArrayRef<RoutineDecl *> getVTable() const { return VTable; }
  const void *getModule() const { return Module; }
  bool isDefined() const { return Defined; }
  llvm::ArrayRef<InterfaceImpl> getInterfaces() const { return Interfaces; }
  void setInterfaces(llvm::ArrayRef<InterfaceImpl> I) { Interfaces = I; }
  /// Does the class list I (itself or through an ancestor)?
  const InterfaceImpl *findInterface(const InterfaceType *I) const {
    for (const InterfaceImpl &X : Interfaces)
      if (X.Intf == I)
        return &X;
    return nullptr;
  }
  void define(ClassType *P, llvm::ArrayRef<Decl *> M,
              llvm::ArrayRef<FieldDecl *> Fields,
              llvm::ArrayRef<RoutineDecl *> VT, const void *Mod) {
    Parent = P;
    Members = M;
    AllFields = Fields;
    VTable = VT;
    Module = Mod;
    Defined = true;
  }
  void setModule(const void *Mod) { Module = Mod; }
  /// The member Name declared in this class itself.
  Decl *findOwnMember(llvm::StringRef Name) const;
  /// The member Name of this class or its nearest ancestor declaring it;
  /// *Owner receives that class.
  Decl *findMember(llvm::StringRef Name, const ClassType **Owner = nullptr) const;
  /// Is this class C or a descendant of C?
  bool inheritsFrom(const ClassType *C) const;
  static bool classof(const Type *T) { return T->getKind() == TK_Class; }
};

/// interface [(Parent)] ... end (objfpc): methods by slot, the parent's
/// first. A value is {object, the object's class's method table for it}.
/// COM interfaces (the default; IUnknown and its descendants) are reference
/// counted: their objects are TInterfacedObjects, freed when the last
/// interface reference to them goes away. CORBA ones are not.
class InterfaceType final : public Type {
  InterfaceType *Parent = nullptr;
  bool COM = false;
  llvm::ArrayRef<RoutineDecl *> Methods;    ///< own, in source order
  llvm::ArrayRef<RoutineDecl *> AllMethods; ///< by slot
  const void *Module = nullptr;
  bool Defined = false;

public:
  InterfaceType() : Type(TK_Interface) {}
  InterfaceType *getParent() const { return Parent; }
  llvm::ArrayRef<RoutineDecl *> getMethods() const { return Methods; }
  llvm::ArrayRef<RoutineDecl *> getAllMethods() const { return AllMethods; }
  const void *getModule() const { return Module; }
  bool isDefined() const { return Defined; }
  bool isCOM() const { return COM; }
  void setCOM(bool C) { COM = C; }
  void define(InterfaceType *P, llvm::ArrayRef<RoutineDecl *> M,
              llvm::ArrayRef<RoutineDecl *> All, const void *Mod) {
    Parent = P;
    Methods = M;
    AllMethods = All;
    Module = Mod;
    Defined = true;
  }
  /// Is this interface I or a descendant of I?
  bool inheritsFrom(const InterfaceType *I) const {
    for (const InterfaceType *X = this; X; X = X->Parent)
      if (X == I)
        return true;
    return false;
  }
  static bool classof(const Type *T) { return T->getKind() == TK_Interface; }
};

/// class of C (objfpc): its values are C and its descendants.
class ClassRefType final : public Type {
  ClassType *Of;

public:
  explicit ClassRefType(ClassType *Of) : Type(TK_ClassRef), Of(Of) {}
  ClassType *getClass() const { return Of; }
  static bool classof(const Type *T) { return T->getKind() == TK_ClassRef; }
};

/// Turbo/FPC: do L and R compare as short strings? (Two chars, or two ISO
/// string types of equal length, keep their own comparisons.)
inline bool needsStringCompare(Type *L, Type *R) {
  if (!L->isStringish() || !R->isStringish())
    return false;
  if (L->getBase()->isChar() && R->getBase()->isChar())
    return false;
  if (L->isStringType() && R->isStringType() &&
      L->getStringLength() == R->getStringLength())
    return false;
  return true;
}

/// True if T is, or contains, a file type (files cannot be copied).
bool containsFile(Type *T);

/// True if T is, or contains, a COM interface or an ansistring (a
/// reference-counted value:
/// managed by the compiled code when assigned, copied and finalized).
bool isManaged(Type *T);

/// True if T is, or contains, a COM interface.
bool containsInterface(Type *T);

/// Bounds of an ordinal type: integer [-2^31, 2^31-1] (other integer types
/// by width and signedness), char [0, 255], boolean [0, 1], enum [0, N-1],
/// subrange [Lo, Hi].
struct OrdinalBounds {
  int64_t Lo, Hi;
};
/// Lo..Hi of ordinal type T. For qword (whose values do not fit int64_t)
/// 0..INT64_MAX: callers that need its whole range test isQWord.
OrdinalBounds getOrdinalBounds(Type *T);
/// Is T qword (unsigned 64-bit), not a subrange of it? Its values are kept
/// as their 64 bits in int64_t.
bool isQWord(const Type *T);

/// Are A and B ordinal types with the same host? All integer types count as
/// one host (Turbo/FPC): they are compatible, and assignment between them is
/// range-checked.
bool haveSameOrdinalHost(Type *A, Type *B);

} // namespace pcc

#endif // PCC_AST_TYPE_H
