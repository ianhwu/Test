//===--- SILVTableVisitor.h - Class vtable visitor --------------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file defines the SILVTableVisitor class, which is used to generate and
// perform lookups in class method vtables.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SIL_SILVTABLEVISITOR_H
#define SWIFT_SIL_SILVTABLEVISITOR_H

#include <string>

#include "swift/AST/Decl.h"
#include "swift/AST/Types.h"
#include "swift/AST/ASTMangler.h"

namespace swift {

// Utility class for deterministically ordering vtable entries for
// synthesized methods.
struct SortedFuncList {
  using Entry = std::pair<std::string, AbstractFunctionDecl *>;
  SmallVector<Entry, 2> elts;
  bool sorted = false;

  void add(AbstractFunctionDecl *afd) {
    Mangle::ASTMangler mangler;
    std::string mangledName;
    if (auto *cd = dyn_cast<ConstructorDecl>(afd))
      mangledName = mangler.mangleConstructorEntity(cd, 0, 0);
    else
      mangledName = mangler.mangleEntity(afd, 0);

    elts.push_back(std::make_pair(mangledName, afd));
  }

  bool empty() { return elts.empty(); }

  void sort() {
    assert(!sorted);
    sorted = true;
    std::sort(elts.begin(),
              elts.end(),
              [](const Entry &lhs, const Entry &rhs) -> bool {
                return lhs.first < rhs.first;
              });
  }

  decltype(elts)::const_iterator begin() const {
    assert(sorted);
    return elts.begin();
  }

  decltype(elts)::const_iterator end() const {
    assert(sorted);
    return elts.end();
  }
};

/// A CRTP class for visiting virtually-dispatched methods of a class.
///
/// You must override these two methods in your subclass:
///
/// - addMethod(SILDeclRef):
///   introduce a new vtable entry
///
/// - addMethodOverride(SILDeclRef baseRef, SILDeclRef derivedRef):
///   update vtable entry for baseRef to call derivedRef
///
/// - addPlaceholder(MissingMemberDecl *);
///   introduce an entry for a method that could not be deserialized
///
template <class T> class SILVTableVisitor {
  T &asDerived() { return *static_cast<T*>(this); }

  void maybeAddMethod(FuncDecl *fd) {
    assert(!fd->hasClangNode());

    maybeAddEntry(SILDeclRef(fd, SILDeclRef::Kind::Func));
  }

  void maybeAddConstructor(ConstructorDecl *cd) {
    assert(!cd->hasClangNode());

    // The allocating entry point is what is used for dynamic dispatch.
    // The initializing entry point for designated initializers is only
    // necessary for super.init chaining, which is sufficiently constrained
    // to never need dynamic dispatch.
    maybeAddEntry(SILDeclRef(cd, SILDeclRef::Kind::Allocator));
  }

  void maybeAddAccessors(AbstractStorageDecl *asd) {
    asd->visitOpaqueAccessors([&](AccessorDecl *accessor) {
      maybeAddMethod(accessor);
    });
  }

  void maybeAddEntry(SILDeclRef declRef) {
    // Introduce a new entry if required.
    if (declRef.requiresNewVTableEntry())
      asDerived().addMethod(declRef);

    // Update any existing entries that it overrides.
    auto nextRef = declRef;
    while ((nextRef = nextRef.getNextOverriddenVTableEntry())) {
      auto baseRef = nextRef.getOverriddenVTableEntry();

      // If A.f() is overridden by B.f() which is overridden by
      // C.f(), it's possible that C.f() is not visible from C.
      // In this case, we pretend that B.f() is the least derived
      // method with a vtable entry in the override chain.
      //
      // This works because we detect the possibility of this
      // happening when we emit B.f() and do two things:
      // - B.f() always gets a new vtable entry, even if it is
      //   ABI compatible with A.f()
      // - The vtable thunk for the override of A.f() in B does a
      //   vtable dispatch to the implementation of B.f() for the
      //   concrete subclass, so a subclass of B only needs to
      //   replace the vtable entry for B.f(); a call to A.f()
      //   will correctly dispatch to the implementation of B.f()
      //   in the subclass.
      if (!baseRef.getDecl()->isAccessibleFrom(
            declRef.getDecl()->getDeclContext()))
        break;

      asDerived().addMethodOverride(baseRef, declRef);
      nextRef = baseRef;
    }
  }

  void maybeAddMember(Decl *member) {
    if (isa<AccessorDecl>(member))
      /* handled as part of its storage */;
    else if (auto *fd = dyn_cast<FuncDecl>(member))
      maybeAddMethod(fd);
    else if (auto *cd = dyn_cast<ConstructorDecl>(member))
      maybeAddConstructor(cd);
    else if (auto *asd = dyn_cast<AbstractStorageDecl>(member))
      maybeAddAccessors(asd);
    else if (auto *placeholder = dyn_cast<MissingMemberDecl>(member))
      asDerived().addPlaceholder(placeholder);
  }

protected:
  void addVTableEntries(ClassDecl *theClass) {
    // Imported classes do not have a vtable.
    if (!theClass->hasKnownSwiftImplementation())
      return;

    // Note that while vtable order is not ABI, we still want it to be
    // consistent between translation units.
    //
    // So, sort synthesized members by their mangled name, since they
    // are added lazily during type checking, with the remaining ones
    // forced at the end.
    SortedFuncList synthesizedMembers;

    for (auto member : theClass->getEmittedMembers()) {
      if (auto *afd = dyn_cast<AbstractFunctionDecl>(member)) {
        if (afd->isSynthesized()) {
          synthesizedMembers.add(afd);
          continue;
        }
      }

      maybeAddMember(member);
    }

    if (synthesizedMembers.empty())
      return;

    synthesizedMembers.sort();

    for (const auto &pair : synthesizedMembers) {
      maybeAddMember(pair.second);
    }
  }
};

}

#endif
