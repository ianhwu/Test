// RUN: %target-typecheck-verify-swift -solver-expression-time-threshold=1 -solver-disable-shrink -disable-constraint-solver-performance-hacks -solver-enable-operator-designated-types
// REQUIRES: tools-release,no_asan

let _: (Character) -> Bool = { c in
  ("a" <= c && c <= "z") || ("A" <= c && c <= "Z") || c == "_"
}
