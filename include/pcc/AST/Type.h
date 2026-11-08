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
    TK_Class,    ///< class ... end (objfpc): a reference to an object
    TK_ClassRef, ///< class of T (objfpc): a reference to a class (its VMT)
    TK_Interface, ///< interface ... end (objfpc): {object, method table}
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
  bool isSet() const { return Kind == TK_Set; }
  bool isFile() const { return Kind == TK_File; }
  bool isShortString() const { return Kind == TK_ShortString; }
  bool isClass() const { return Kind == TK_Class; }
  bool isClassRef() const { return Kind == TK_ClassRef; }
  bool isInterface() const { return Kind == TK_Interface; }
  /// Anything that converts to a short string: short strings, string types,
  /// and chars (Turbo/FPC).
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
           isInterface();
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
    case TK_Nil:
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

public:
  ArrayType(Type *Index, Type *Element, bool Packed)
      : Type(TK_Array), Index(Index), Element(Element), Packed(Packed) {}
  Type *getIndexType() const { return Index; }
  Type *getElementType() const { return Element; }
  bool isPacked() const { return Packed; }
  static bool classof(const Type *T) { return T->getKind() == TK_Array; }
};

/// [packed] record ... end. The layout follows the FieldList tree (fixed
/// part, then an optional variant part whose variants overlay each other).
class RecordType final : public Type {
  FieldList *Fields;
  llvm::ArrayRef<FieldDecl *> AllFields; ///< flattened, incl. variant fields
  bool Packed;

public:
  RecordType(FieldList *Fields, llvm::ArrayRef<FieldDecl *> AllFields,
             bool Packed)
      : Type(TK_Record), Fields(Fields), AllFields(AllFields), Packed(Packed) {}
  FieldList *getFieldList() const { return Fields; }
  llvm::ArrayRef<FieldDecl *> getAllFields() const { return AllFields; }
  FieldDecl *findField(llvm::StringRef Name) const;
  bool isPacked() const { return Packed; }
  static bool classof(const Type *T) { return T->getKind() == TK_Record; }
};

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

/// The type of a procedural (Result == null) or functional parameter.
class RoutineType final : public Type {
  llvm::ArrayRef<VarDecl *> Params;
  Type *Result;

public:
  RoutineType(llvm::ArrayRef<VarDecl *> Params, Type *Result)
      : Type(TK_Routine), Params(Params), Result(Result) {}
  llvm::ArrayRef<VarDecl *> getParams() const { return Params; }
  Type *getResult() const { return Result; }
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

public:
  ClassType() : Type(TK_Class) {}
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
class InterfaceType final : public Type {
  InterfaceType *Parent = nullptr;
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

/// Bounds of an ordinal type: integer [-2^31, 2^31-1] (other integer types
/// by width and signedness), char [0, 255], boolean [0, 1], enum [0, N-1],
/// subrange [Lo, Hi].
struct OrdinalBounds {
  int64_t Lo, Hi;
};
OrdinalBounds getOrdinalBounds(Type *T);

/// Are A and B ordinal types with the same host? All integer types count as
/// one host (Turbo/FPC): they are compatible, and assignment between them is
/// range-checked.
bool haveSameOrdinalHost(Type *A, Type *B);

} // namespace pcc

#endif // PCC_AST_TYPE_H
