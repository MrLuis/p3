//===- Type.h - Semantic types ------------------------------------*- C++ -*-===//
//
// Types as Sema understands them (as opposed to TypeSpec, which is how a type
// is *written*). Types live in the ASTContext arena. ISO Pascal uses name
// equivalence: every `array`/`record`/enumeration written in the source is a
// distinct type, so type identity is pointer identity.
//
//   simple:     integer, real, boolean, char, enumerations, subranges
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
class FieldDecl;
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
  bool isTextFile() const;

  /// ISO 7185 6.4.2.1: integer, boolean, char, enumerations and subranges.
  bool isOrdinal() const {
    return Kind == TK_Integer || Kind == TK_Boolean || Kind == TK_Char ||
           Kind == TK_Enum || Kind == TK_Subrange;
  }
  bool isSimple() const { return isOrdinal() || isReal(); }
  /// Types a function may return (ISO 6.6.2: simple or pointer).
  bool isValidResultType() const { return isSimple() || isPointer(); }
  bool isStructured() const { return isArray() || isRecord(); }

  /// Integer-like after stripping subranges (integer or a subrange of it).
  bool isIntegerLike() const;
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
class BuiltinType final : public Type {
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

public:
  explicit PointerType(Type *Pointee) : Type(TK_Pointer), Pointee(Pointee) {}
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

/// True if T is, or contains, a file type (files cannot be copied).
bool containsFile(Type *T);

/// Bounds of an ordinal type: integer [-2^31, 2^31-1], char [0, 255],
/// boolean [0, 1], enum [0, N-1], subrange [Lo, Hi].
struct OrdinalBounds {
  int64_t Lo, Hi;
};
OrdinalBounds getOrdinalBounds(Type *T);

} // namespace pcc

#endif // PCC_AST_TYPE_H
