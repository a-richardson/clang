//===- Chrootchecker.cpp -------- Basic security checks ----------*- C++ -*-==//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
//  This file defines chroot checker, which checks improper use of chroot.
//
//===----------------------------------------------------------------------===//

#include "ExprEngineExperimentalChecks.h"
#include "clang/StaticAnalyzer/BugReporter/BugType.h"
#include "clang/StaticAnalyzer/PathSensitive/CheckerVisitor.h"
#include "clang/StaticAnalyzer/PathSensitive/GRState.h"
#include "clang/StaticAnalyzer/PathSensitive/GRStateTrait.h"
#include "clang/StaticAnalyzer/PathSensitive/SymbolManager.h"
#include "llvm/ADT/ImmutableMap.h"
using namespace clang;
using namespace ento;

namespace {

// enum value that represent the jail state
enum Kind { NO_CHROOT, ROOT_CHANGED, JAIL_ENTERED };
  
bool isRootChanged(intptr_t k) { return k == ROOT_CHANGED; }
//bool isJailEntered(intptr_t k) { return k == JAIL_ENTERED; }

// This checker checks improper use of chroot.
// The state transition:
// NO_CHROOT ---chroot(path)--> ROOT_CHANGED ---chdir(/) --> JAIL_ENTERED
//                                  |                               |
//         ROOT_CHANGED<--chdir(..)--      JAIL_ENTERED<--chdir(..)--
//                                  |                               |
//                      bug<--foo()--          JAIL_ENTERED<--foo()--
class ChrootChecker : public CheckerVisitor<ChrootChecker> {
  IdentifierInfo *II_chroot, *II_chdir;
  // This bug refers to possibly break out of a chroot() jail.
  BuiltinBug *BT_BreakJail;

public:
  ChrootChecker() : II_chroot(0), II_chdir(0), BT_BreakJail(0) {}
  
  static void *getTag() {
    static int x;
    return &x;
  }
  
  virtual bool evalCallExpr(CheckerContext &C, const CallExpr *CE);
  virtual void PreVisitCallExpr(CheckerContext &C, const CallExpr *CE);

private:
  void Chroot(CheckerContext &C, const CallExpr *CE);
  void Chdir(CheckerContext &C, const CallExpr *CE);
};

} // end anonymous namespace

void ento::RegisterChrootChecker(ExprEngine &Eng) {
  Eng.registerCheck(new ChrootChecker());
}

bool ChrootChecker::evalCallExpr(CheckerContext &C, const CallExpr *CE) {
  const GRState *state = C.getState();
  const Expr *Callee = CE->getCallee();
  SVal L = state->getSVal(Callee);
  const FunctionDecl *FD = L.getAsFunctionDecl();
  if (!FD)
    return false;

  ASTContext &Ctx = C.getASTContext();
  if (!II_chroot)
    II_chroot = &Ctx.Idents.get("chroot");
  if (!II_chdir)
    II_chdir = &Ctx.Idents.get("chdir");

  if (FD->getIdentifier() == II_chroot) {
    Chroot(C, CE);
    return true;
  }
  if (FD->getIdentifier() == II_chdir) {
    Chdir(C, CE);
    return true;
  }

  return false;
}

void ChrootChecker::Chroot(CheckerContext &C, const CallExpr *CE) {
  const GRState *state = C.getState();
  GRStateManager &Mgr = state->getStateManager();
  
  // Once encouter a chroot(), set the enum value ROOT_CHANGED directly in 
  // the GDM.
  state = Mgr.addGDM(state, ChrootChecker::getTag(), (void*) ROOT_CHANGED);
  C.addTransition(state);
}

void ChrootChecker::Chdir(CheckerContext &C, const CallExpr *CE) {
  const GRState *state = C.getState();
  GRStateManager &Mgr = state->getStateManager();

  // If there are no jail state in the GDM, just return.
  const void* k = state->FindGDM(ChrootChecker::getTag());
  if (!k)
    return;

  // After chdir("/"), enter the jail, set the enum value JAIL_ENTERED.
  const Expr *ArgExpr = CE->getArg(0);
  SVal ArgVal = state->getSVal(ArgExpr);
  
  if (const MemRegion *R = ArgVal.getAsRegion()) {
    R = R->StripCasts();
    if (const StringRegion* StrRegion= dyn_cast<StringRegion>(R)) {
      const StringLiteral* Str = StrRegion->getStringLiteral();
      if (Str->getString() == "/")
        state = Mgr.addGDM(state, ChrootChecker::getTag(),
                           (void*) JAIL_ENTERED);
    }
  }

  C.addTransition(state);
}

// Check the jail state before any function call except chroot and chdir().
void ChrootChecker::PreVisitCallExpr(CheckerContext &C, const CallExpr *CE) {
  const GRState *state = C.getState();
  const Expr *Callee = CE->getCallee();
  SVal L = state->getSVal(Callee);
  const FunctionDecl *FD = L.getAsFunctionDecl();
  if (!FD)
    return;

  ASTContext &Ctx = C.getASTContext();
  if (!II_chroot)
    II_chroot = &Ctx.Idents.get("chroot");
  if (!II_chdir)
    II_chdir = &Ctx.Idents.get("chdir");

  // Ingnore chroot and chdir.
  if (FD->getIdentifier() == II_chroot || FD->getIdentifier() == II_chdir)
    return;
  
  // If jail state is ROOT_CHANGED, generate BugReport.
  void* const* k = state->FindGDM(ChrootChecker::getTag());
  if (k)
    if (isRootChanged((intptr_t) *k))
      if (ExplodedNode *N = C.generateNode()) {
        if (!BT_BreakJail)
          BT_BreakJail = new BuiltinBug("Break out of jail",
                                        "No call of chdir(\"/\") immediately "
                                        "after chroot");
        BugReport *R = new BugReport(*BT_BreakJail, 
                                     BT_BreakJail->getDescription(), N);
        C.EmitReport(R);
      }
  
  return;
}
