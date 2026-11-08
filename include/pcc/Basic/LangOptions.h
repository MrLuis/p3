//===- LangOptions.h - Pascal dialect switches --------------------*- C++ -*-===//
#ifndef PCC_BASIC_LANGOPTIONS_H
#define PCC_BASIC_LANGOPTIONS_H

namespace pcc {

/// Accepted language dialect (--mode=).
enum class LangMode {
  ISO,    ///< ISO 7185 Standard Pascal
  TP,     ///< Turbo / Free Pascal extensions
  ObjFPC, ///< TP plus classes (Free Pascal's objfpc mode)
};

/// Fine-grained feature switches derived from the dialect. Front-end stages
/// test these flags rather than the mode itself.
struct LangOptions {
  LangMode Mode = LangMode::ISO;
  /// Turbo/FPC reserved words (unit, string, shl, ...) and '@' as address-of.
  bool TPKeywords = false;
  /// Object Pascal reserved words (class, inherited, is, as, property).
  bool ObjKeywords = false;
  /// '' (empty string literal) is allowed.
  bool EmptyStrings = false;

  static LangOptions forMode(LangMode M) {
    LangOptions LO;
    LO.Mode = M;
    bool TP = M != LangMode::ISO;
    LO.TPKeywords = TP;
    LO.ObjKeywords = M == LangMode::ObjFPC;
    LO.EmptyStrings = TP;
    return LO;
  }
};

} // namespace pcc

#endif // PCC_BASIC_LANGOPTIONS_H
