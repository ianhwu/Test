// RUN: %target-typecheck-verify-swift

@noescape var fn : () -> Int = { 4 }  // expected-error {{attribute can only be applied to types, not declarations}}

func conflictingAttrs(_ fn: @noescape @escaping () -> Int) {} // expected-error {{unknown attribute 'noescape'}}

func doesEscape(_ fn : @escaping () -> Int) {}

func takesGenericClosure<T>(_ a : Int, _ fn : @noescape () -> T) {} // expected-error 2{{unknown attribute 'noescape'}}


var globalAny: Any = 0

func assignToGlobal<T>(_ t: T) {
  globalAny = t
}

func takesArray(_ fns: [() -> Int]) {
  doesEscape(fns[0]) // Okay - array-of-function parameters are escaping
}

func takesVariadic(_ fns: () -> Int...) {
  doesEscape(fns[0]) // Okay - variadic-of-function parameters are escaping
}

func takesNoEscapeClosure(_ fn : () -> Int) {
  // expected-note@-1 5{{parameter 'fn' is implicitly non-escaping}} {{34-34=@escaping }}
  takesNoEscapeClosure { 4 }  // ok

  _ = fn()  // ok

  // This is ok, because the closure itself is noescape.
  takesNoEscapeClosure { fn() }

  doesEscape(fn)                   // expected-error {{passing non-escaping parameter 'fn' to function expecting an @escaping closure}}
  takesGenericClosure(4, fn)       // ok
  takesGenericClosure(4) { fn() }  // ok.

  _ = [fn] // expected-error {{converting non-escaping value to 'Element' may allow it to escape}}
  _ = [doesEscape(fn)] // expected-error {{passing non-escaping parameter 'fn' to function expecting an @escaping closure}}
  _ = [1 : fn] // expected-error {{converting non-escaping value to 'Value' may allow it to escape}}
  _ = [1 : doesEscape(fn)] // expected-error {{passing non-escaping parameter 'fn' to function expecting an @escaping closure}}
  _ = "\(doesEscape(fn))" // expected-error {{passing non-escaping parameter 'fn' to function expecting an @escaping closure}}
  _ = "\(takesArray([fn]))" // expected-error {{using non-escaping parameter 'fn' in a context expecting an @escaping closure}}

  assignToGlobal(fn) // expected-error {{converting non-escaping value to 'T' may allow it to escape}}
  assignToGlobal((fn, fn)) // expected-error {{converting non-escaping value to 'T' may allow it to escape}}
}

class SomeClass {
  final var x = 42

  func test() {
    // This should require "self."
    doesEscape { x }  // expected-error {{reference to property 'x' in closure requires explicit 'self.' to make capture semantics explicit}} {{18-18=self.}}

    // Since 'takesNoEscapeClosure' doesn't escape its closure, it doesn't
    // require "self." qualification of member references.
    takesNoEscapeClosure { x }
  }

  @discardableResult
  func foo() -> Int {
    foo()

    func plain() { foo() }
    let plain2 = { foo() } // expected-error {{call to method 'foo' in closure requires explicit 'self.' to make capture semantics explicit}} {{20-20=self.}}
    _ = plain2

    func multi() -> Int { foo(); return 0 }
    let mulit2: () -> Int = { foo(); return 0 } // expected-error {{call to method 'foo' in closure requires explicit 'self.' to make capture semantics explicit}} {{31-31=self.}}
    _ = mulit2

    doesEscape { foo() } // expected-error {{call to method 'foo' in closure requires explicit 'self.' to make capture semantics explicit}} {{18-18=self.}}
    takesNoEscapeClosure { foo() } // okay

    doesEscape { foo(); return 0 } // expected-error {{call to method 'foo' in closure requires explicit 'self.' to make capture semantics explicit}} {{18-18=self.}}
    takesNoEscapeClosure { foo(); return 0 } // okay

    func outer() {
      func inner() { foo() }
      let inner2 = { foo() } // expected-error {{call to method 'foo' in closure requires explicit 'self.' to make capture semantics explicit}} {{22-22=self.}}
      _ = inner2
      func multi() -> Int { foo(); return 0 }
      let _: () -> Int = { foo(); return 0 } // expected-error {{call to method 'foo' in closure requires explicit 'self.' to make capture semantics explicit}} {{28-28=self.}}
      doesEscape { foo() } // expected-error {{call to method 'foo' in closure requires explicit 'self.' to make capture semantics explicit}} {{20-20=self.}}
      takesNoEscapeClosure { foo() }
      doesEscape { foo(); return 0 } // expected-error {{call to method 'foo' in closure requires explicit 'self.' to make capture semantics explicit}} {{20-20=self.}}
      takesNoEscapeClosure { foo(); return 0 }
    }

    let _: () -> Void = {
      func inner() { foo() } // expected-error {{call to method 'foo' in closure requires explicit 'self.' to make capture semantics explicit}} {{22-22=self.}}
      let inner2 = { foo() } // expected-error {{call to method 'foo' in closure requires explicit 'self.' to make capture semantics explicit}} {{22-22=self.}}
      _ = inner2
      func multi() -> Int { foo(); return 0 } // expected-error {{call to method 'foo' in closure requires explicit 'self.' to make capture semantics explicit}} {{29-29=self.}}
      let mulit2: () -> Int = { foo(); return 0 } // expected-error {{call to method 'foo' in closure requires explicit 'self.' to make capture semantics explicit}} {{33-33=self.}}
      _ = mulit2
      doesEscape { foo() }  // expected-error {{call to method 'foo' in closure requires explicit 'self.' to make capture semantics explicit}} {{20-20=self.}}
      takesNoEscapeClosure { foo() }  // expected-error {{call to method 'foo' in closure requires explicit 'self.' to make capture semantics explicit}} {{30-30=self.}}
      doesEscape { foo(); return 0 }  // expected-error {{call to method 'foo' in closure requires explicit 'self.' to make capture semantics explicit}} {{20-20=self.}}
      takesNoEscapeClosure { foo(); return 0 }  // expected-error {{call to method 'foo' in closure requires explicit 'self.' to make capture semantics explicit}} {{30-30=self.}}
    }

    doesEscape {
      func inner() { foo() }  // expected-error {{call to method 'foo' in closure requires explicit 'self.' to make capture semantics explicit}} {{22-22=self.}}
      let inner2 = { foo() }  // expected-error {{call to method 'foo' in closure requires explicit 'self.' to make capture semantics explicit}} {{22-22=self.}}
      _ = inner2
      func multi() -> Int { foo(); return 0 }  // expected-error {{call to method 'foo' in closure requires explicit 'self.' to make capture semantics explicit}} {{29-29=self.}}
      let mulit2: () -> Int = { foo(); return 0 }  // expected-error {{call to method 'foo' in closure requires explicit 'self.' to make capture semantics explicit}} {{33-33=self.}}
      _ = mulit2
      doesEscape { foo() }  // expected-error {{call to method 'foo' in closure requires explicit 'self.' to make capture semantics explicit}} {{20-20=self.}}
      takesNoEscapeClosure { foo() }  // expected-error {{call to method 'foo' in closure requires explicit 'self.' to make capture semantics explicit}} {{30-30=self.}}
      doesEscape { foo(); return 0 }  // expected-error {{call to method 'foo' in closure requires explicit 'self.' to make capture semantics explicit}} {{20-20=self.}}
      takesNoEscapeClosure { foo(); return 0 }  // expected-error {{call to method 'foo' in closure requires explicit 'self.' to make capture semantics explicit}} {{30-30=self.}}
      return 0
    }
    takesNoEscapeClosure {
      func inner() { foo() }
      let inner2 = { foo() }  // expected-error {{call to method 'foo' in closure requires explicit 'self.' to make capture semantics explicit}} {{22-22=self.}}
      _ = inner2
      func multi() -> Int { foo(); return 0 }
      let mulit2: () -> Int = { foo(); return 0 }  // expected-error {{call to method 'foo' in closure requires explicit 'self.' to make capture semantics explicit}} {{33-33=self.}}
      _ = mulit2
      doesEscape { foo() } // expected-error {{call to method 'foo' in closure requires explicit 'self.' to make capture semantics explicit}} {{20-20=self.}}
      takesNoEscapeClosure { foo() } // okay
      doesEscape { foo(); return 0 } // expected-error {{call to method 'foo' in closure requires explicit 'self.' to make capture semantics explicit}} {{20-20=self.}}
      takesNoEscapeClosure { foo(); return 0 } // okay
      return 0
    }

    struct Outer {
      @discardableResult
      func bar() -> Int {
        bar()

        func plain() { bar() }
        let plain2 = { bar() } // expected-error {{call to method 'bar' in closure requires explicit 'self.' to make capture semantics explicit}} {{24-24=self.}}
        _ = plain2

        func multi() -> Int { bar(); return 0 }
        let _: () -> Int = { bar(); return 0 } // expected-error {{call to method 'bar' in closure requires explicit 'self.' to make capture semantics explicit}} {{30-30=self.}}

        doesEscape { bar() } // expected-error {{call to method 'bar' in closure requires explicit 'self.' to make capture semantics explicit}} {{22-22=self.}}
        takesNoEscapeClosure { bar() } // okay

        doesEscape { bar(); return 0 } // expected-error {{call to method 'bar' in closure requires explicit 'self.' to make capture semantics explicit}} {{22-22=self.}}
        takesNoEscapeClosure { bar(); return 0 } // okay

        return 0
      }
    }

    func structOuter() {
      struct Inner {
        @discardableResult
        func bar() -> Int {
          bar() // no-warning

          func plain() { bar() }
          let plain2 = { bar() } // expected-error {{call to method 'bar' in closure requires explicit 'self.' to make capture semantics explicit}} {{26-26=self.}}
          _ = plain2

          func multi() -> Int { bar(); return 0 }
          let _: () -> Int = { bar(); return 0 } // expected-error {{call to method 'bar' in closure requires explicit 'self.' to make capture semantics explicit}} {{32-32=self.}}

          doesEscape { bar() } // expected-error {{call to method 'bar' in closure requires explicit 'self.' to make capture semantics explicit}} {{24-24=self.}}
          takesNoEscapeClosure { bar() } // okay

          doesEscape { bar(); return 0 } // expected-error {{call to method 'bar' in closure requires explicit 'self.' to make capture semantics explicit}} {{24-24=self.}}
          takesNoEscapeClosure { bar(); return 0 } // okay

          return 0
        }
      }
    }

    return 0
  }
}


// Implicit conversions (in this case to @convention(block)) are ok.
@_silgen_name("whatever") 
func takeNoEscapeAsObjCBlock(_: @noescape @convention(block) () -> Void)  // expected-error{{unknown attribute 'noescape'}}
func takeNoEscapeTest2(_ fn : @noescape () -> ()) {  // expected-error{{unknown attribute 'noescape'}}
  takeNoEscapeAsObjCBlock(fn)
}

// Autoclosure implies noescape..
func testAutoclosure(_ a : @autoclosure () -> Int) { // expected-note{{parameter 'a' is implicitly non-escaping}}
  doesEscape(a)  // expected-error {{passing non-escaping parameter 'a' to function expecting an @escaping closure}}
}


// <rdar://problem/19470858> QoI: @autoclosure implies @noescape, so you shouldn't be allowed to specify both
func redundant(_ fn : @noescape
               @autoclosure () -> Int) {
 // expected-error@-2{{unknown attribute 'noescape'}}
}


protocol P1 {
  associatedtype Element
}
protocol P2 : P1 {
  associatedtype Element
}

func overloadedEach<O: P1, T>(_ source: O, _ transform: @escaping (O.Element) -> (), _: T) {}

func overloadedEach<P: P2, T>(_ source: P, _ transform: @escaping (P.Element) -> (), _: T) {}

struct S : P2 {
  typealias Element = Int
  func each(_ transform: @noescape (Int) -> ()) { // expected-error{{unknown attribute 'noescape'}}
                                                  // expected-note@-1 {{parameter 'transform' is implicitly non-escaping}}
    overloadedEach(self, transform, 1)
    // expected-error@-1 {{passing non-escaping parameter 'transform' to function expecting an @escaping closure}}
  }
}



// rdar://19763676 - False positive in @noescape analysis triggered by parameter label
func r19763676Callee(_ f: @noescape (_ param: Int) -> Int) {} // expected-error{{unknown attribute 'noescape'}}

func r19763676Caller(_ g: @noescape (Int) -> Int) { // expected-error{{unknown attribute 'noescape'}}
  r19763676Callee({ _ in g(1) })
}


// <rdar://problem/19763732> False positive in @noescape analysis triggered by default arguments
func calleeWithDefaultParameters(_ f: @noescape () -> (), x : Int = 1) {} // expected-error{{unknown attribute 'noescape'}}
func callerOfDefaultParams(_ g: @noescape () -> ()) { // expected-error{{unknown attribute 'noescape'}}
  calleeWithDefaultParameters(g)
}



// <rdar://problem/19773562> Closures executed immediately { like this }() are not automatically @noescape
class NoEscapeImmediatelyApplied {
  func f() {
    // Shouldn't require "self.", the closure is obviously @noescape.
    _ = { return ivar }()
  }
  
  final var ivar  = 42
}



// Reduced example from XCTest overlay, involves a TupleShuffleExpr
public func XCTAssertTrue(_ expression: @autoclosure () -> Bool, _ message: String = "", file: StaticString = #file, line: UInt = #line) -> Void {
}
public func XCTAssert(_ expression: @autoclosure () -> Bool, _ message: String = "", file: StaticString = #file, line: UInt = #line)  -> Void {
  XCTAssertTrue(expression, message, file: file, line: line);
}



/// SR-770 - Currying and `noescape`/`rethrows` don't work together anymore
func curriedFlatMap<A, B>(_ x: [A]) -> (@noescape (A) -> [B]) -> [B] { // expected-error 2{{unknown attribute 'noescape'}}
  return { f in
    x.flatMap(f)
  }
}

func curriedFlatMap2<A, B>(_ x: [A]) -> (@noescape (A) -> [B]) -> [B] { // expected-error 2{{unknown attribute 'noescape'}}
  return { (f : @noescape (A) -> [B]) in // expected-error{{unknown attribute 'noescape'}}
    x.flatMap(f)
  }
}

func bad(_ a : @escaping (Int)-> Int) -> Int { return 42 }
func escapeNoEscapeResult(_ x: [Int]) -> (@noescape (Int) -> Int) -> Int { // expected-error{{unknown attribute 'noescape'}}
  return { f in // expected-note {{parameter 'f' is implicitly non-escaping}}
    bad(f) // expected-error {{passing non-escaping parameter 'f' to function expecting an @escaping closure}}
  }
}


// SR-824 - @noescape for Type Aliased Closures
//

// Old syntax -- @noescape is the default, and is redundant
typealias CompletionHandlerNE = @noescape (_ success: Bool) -> () // expected-error{{unknown attribute 'noescape'}}

// Explicit @escaping is not allowed here
typealias CompletionHandlerE = @escaping (_ success: Bool) -> () // expected-error{{@escaping attribute may only be used in function parameter position}} {{32-42=}}

// No @escaping -- it's implicit from context
typealias CompletionHandler = (_ success: Bool) -> ()

var escape : CompletionHandlerNE
var escapeOther : CompletionHandler
func doThing1(_ completion: (_ success: Bool) -> ()) {
  // expected-note@-1 {{parameter 'completion' is implicitly non-escaping}}
  escape = completion // expected-error {{assigning non-escaping parameter 'completion' to an @escaping closure}}
}
func doThing2(_ completion: CompletionHandlerNE) {
  // expected-note@-1 {{parameter 'completion' is implicitly non-escaping}}
  escape = completion // expected-error {{assigning non-escaping parameter 'completion' to an @escaping closure}}
}
func doThing3(_ completion: CompletionHandler) {
  // expected-note@-1 {{parameter 'completion' is implicitly non-escaping}}
  escape = completion // expected-error {{assigning non-escaping parameter 'completion' to an @escaping closure}}
}
func doThing4(_ completion: @escaping CompletionHandler) {
  escapeOther = completion
}

// <rdar://problem/19997680> @noescape doesn't work on parameters of function type
func apply<T, U>(_ f: @noescape (T) -> U, g: @noescape (@noescape (T) -> U) -> U) -> U { 
  // expected-error@-1 6{{unknown attribute 'noescape'}}
  return g(f)
}

// <rdar://problem/19997577> @noescape cannot be applied to locals, leading to duplication of code
enum r19997577Type {
  case Unit
  case Function(() -> r19997577Type, () -> r19997577Type)
  case Sum(() -> r19997577Type, () -> r19997577Type)

  func reduce<Result>(_ initial: Result, _ combine: @noescape (Result, r19997577Type) -> Result) -> Result { // expected-error 2{{unknown attribute 'noescape'}}
    let binary: @noescape (r19997577Type, r19997577Type) -> Result = { combine(combine(combine(initial, self), $0), $1) } // expected-error{{unknown attribute 'noescape'}}
    switch self {
    case .Unit:
      return combine(initial, self)
    case let .Function(t1, t2):
      return binary(t1(), t2())
    case let .Sum(t1, t2):
      return binary(t1(), t2())
    }
  }
}

// type attribute and decl attribute
func noescapeD(@noescape f: @escaping () -> Bool) {} // expected-error {{attribute can only be applied to types, not declarations}}
func noescapeT(f: @noescape () -> Bool) {} // expected-error{{unknown attribute 'noescape'}}
func noescapeG<T>(@noescape f: () -> T) {} // expected-error{{attribute can only be applied to types, not declarations}}

func autoclosureD(@autoclosure f: () -> Bool) {} // expected-error {{attribute can only be applied to types, not declarations}}
func autoclosureT(f: @autoclosure () -> Bool) {}  // ok
func autoclosureG<T>(@autoclosure f: () -> T) {} // expected-error{{attribute can only be applied to types, not declarations}}

func noescapeD_noescapeT(@noescape f: @noescape () -> Bool) {} // expected-error {{attribute can only be applied to types, not declarations}}
 // expected-error@-1{{unknown attribute 'noescape'}}

func autoclosureD_noescapeT(@autoclosure f: @noescape () -> Bool) {} // expected-error {{attribute can only be applied to types, not declarations}}
 // expected-error@-1{{unknown attribute 'noescape'}}
