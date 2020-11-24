// RUN: %target-typecheck-verify-swift

func myMap<T1, T2>(_ array: [T1], _ fn: (T1) -> T2) -> [T2] {}

var intArray : [Int]

_ = myMap(intArray, { String($0) })
_ = myMap(intArray, { x -> String in String(x) } )

// Closures with too few parameters.
func foo(_ x: (Int, Int) -> Int) {}
foo({$0}) // expected-error{{contextual closure type '(Int, Int) -> Int' expects 2 arguments, but 1 was used in closure body}}
foo({ [intArray] in $0}) // expected-error{{contextual closure type '(Int, Int) -> Int' expects 2 arguments, but 1 was used in closure body}}

struct X {}
func mySort(_ array: [String], _ predicate: (String, String) -> Bool) -> [String] {}
func mySort(_ array: [X], _ predicate: (X, X) -> Bool) -> [X] {}
var strings : [String]
_ = mySort(strings, { x, y in x < y })

// Closures with inout arguments.
func f0<T, U>(_ t: T, _ f: (inout T) -> U) -> U {
  var t2 = t
  return f(&t2)
}

struct X2 {
  func g() -> Float { return 0 }  
}

_ = f0(X2(), {$0.g()})

// Closures with inout arguments and '__shared' conversions.

func inoutToSharedConversions() {
  func fooOW<T, U>(_ f : (__owned T) -> U) {}
  fooOW({ (x : Int) in return Int(5) }) // defaut-to-'__owned' allowed
  fooOW({ (x : __owned Int) in return Int(5) }) // '__owned'-to-'__owned' allowed
  fooOW({ (x : __shared Int) in return Int(5) }) // '__shared'-to-'__owned' allowed
  fooOW({ (x : inout Int) in return Int(5) }) // expected-error {{cannot convert value of type '(inout Int) -> Int' to expected argument type '(__owned Int) -> Int'}}
  
  func fooIO<T, U>(_ f : (inout T) -> U) {}
  fooIO({ (x : inout Int) in return Int(5) }) // 'inout'-to-'inout' allowed
  fooIO({ (x : Int) in return Int(5) }) // expected-error {{cannot convert value of type '(Int) -> Int' to expected argument type '(inout Int) -> Int'}}
  fooIO({ (x : __shared Int) in return Int(5) }) // expected-error {{cannot convert value of type '(__shared Int) -> Int' to expected argument type '(inout Int) -> Int'}}
  fooIO({ (x : __owned Int) in return Int(5) }) // expected-error {{cannot convert value of type '(__owned Int) -> Int' to expected argument type '(inout Int) -> Int'}}

  func fooSH<T, U>(_ f : (__shared T) -> U) {}
  fooSH({ (x : __shared Int) in return Int(5) }) // '__shared'-to-'__shared' allowed
  fooSH({ (x : __owned Int) in return Int(5) }) // '__owned'-to-'__shared' allowed
  fooSH({ (x : inout Int) in return Int(5) }) // expected-error {{cannot convert value of type '(inout Int) -> Int' to expected argument type '(__shared Int) -> Int'}}
  fooSH({ (x : Int) in return Int(5) }) // default-to-'__shared' allowed
}

// Autoclosure
func f1(f: @autoclosure () -> Int) { }
func f2() -> Int { }
f1(f: f2) // expected-error{{add () to forward @autoclosure parameter}}{{9-9=()}}
f1(f: 5)

// Ternary in closure
var evenOrOdd : (Int) -> String = {$0 % 2 == 0 ? "even" : "odd"}

// <rdar://problem/15367882>
func foo() {
  not_declared({ $0 + 1 }) // expected-error{{use of unresolved identifier 'not_declared'}}
}

// <rdar://problem/15536725>
struct X3<T> {
  init(_: (T) -> ()) {}
}

func testX3(_ x: Int) {
  var x = x
  _ = X3({ x = $0 })
  _ = x
}

// <rdar://problem/13811882>
func test13811882() {
  var _ : (Int) -> (Int, Int) = {($0, $0)}
  var x = 1
  var _ : (Int) -> (Int, Int) = {($0, x)}
  x = 2
}


// <rdar://problem/21544303> QoI: "Unexpected trailing closure" should have a fixit to insert a 'do' statement
// <https://bugs.swift.org/browse/SR-3671>
func r21544303() {
  var inSubcall = true
  {
  }  // expected-error {{computed property must have accessors specified}}
  inSubcall = false

  // This is a problem, but isn't clear what was intended.
  var somethingElse = true {
  }  // expected-error {{computed property must have accessors specified}}
  inSubcall = false

  var v2 : Bool = false
  v2 = inSubcall // expected-error {{cannot call value of non-function type 'Bool'}}
  {
  }
}


// <https://bugs.swift.org/browse/SR-3671>
func SR3671() {
  let n = 42
  func consume(_ x: Int) {}

  { consume($0) }(42)
  ;

  ({ $0(42) } { consume($0) }) // expected-error {{cannot call value of non-function type '()'}} expected-note {{callee is here}}

  { print(42) }  // expected-warning {{braces here form a trailing closure separated from its callee by multiple newlines}} expected-note {{did you mean to use a 'do' statement?}} {{3-3=do }}
  ;

  ({ $0(42) } { consume($0) }) // expected-error {{cannot call value of non-function type '()'}} expected-note {{callee is here}}

  { print($0) }  // expected-warning {{braces here form a trailing closure separated from its callee by multiple newlines}}
  ;

  ({ $0(42) } { consume($0) }) // expected-error {{cannot call value of non-function type '()'}} expected-note {{callee is here}}

  { [n] in print(42) }  // expected-warning {{braces here form a trailing closure separated from its callee by multiple newlines}}
  ;

  ({ $0(42) } { consume($0) }) // expected-error {{cannot call value of non-function type '()'}} expected-note {{callee is here}}

  { consume($0) }(42)  // expected-warning {{braces here form a trailing closure separated from its callee by multiple newlines}}
  ;

  ({ $0(42) } { consume($0) }) // expected-error {{cannot call value of non-function type '()'}} expected-note {{callee is here}}

  { (x: Int) in consume(x) }(42)  // expected-warning {{braces here form a trailing closure separated from its callee by multiple newlines}}
  ;

  // This is technically a valid call, so nothing goes wrong until (42)

  { $0(3) } // expected-error {{cannot call value of non-function type '()'}}
  { consume($0) }(42)
  ;
  ({ $0(42) }) // expected-error {{cannot call value of non-function type '()'}}
  { consume($0) }(42)
  ;
  { $0(3) } // expected-error {{cannot call value of non-function type '()'}}
  { [n] in consume($0) }(42)
  ;
  ({ $0(42) }) // expected-error {{cannot call value of non-function type '()'}}
  { [n] in consume($0) }(42)
  ;

  // Equivalent but more obviously unintended.

  { $0(3) }  // expected-error {{cannot call value of non-function type '()'}} expected-note {{callee is here}}

  { consume($0) }(42)
  // expected-warning@-1 {{braces here form a trailing closure separated from its callee by multiple newlines}}

  ({ $0(3) })  // expected-error {{cannot call value of non-function type '()'}} expected-note {{callee is here}}

  { consume($0) }(42)
  // expected-warning@-1 {{braces here form a trailing closure separated from its callee by multiple newlines}}
  ;

  // Also a valid call (!!)
  { $0 { $0 } } { $0 { 1 } }  // expected-error {{expression resolves to an unused function}}
  consume(111)
}


// <rdar://problem/22162441> Crash from failing to diagnose nonexistent method access inside closure
func r22162441(_ lines: [String]) {
  _ = lines.map { line in line.fooBar() }  // expected-error {{value of type 'String' has no member 'fooBar'}}
  _ = lines.map { $0.fooBar() }  // expected-error {{value of type 'String' has no member 'fooBar'}}
}


func testMap() {
  let a = 42
  [1,a].map { $0 + 1.0 } // expected-error {{cannot convert value of type 'Int' to expected element type 'Double'}}
}

// <rdar://problem/22414757> "UnresolvedDot" "in wrong phase" assertion from verifier
[].reduce { $0 + $1 }  // expected-error {{cannot invoke 'reduce' with an argument list of type '(@escaping (_, _) -> _)'}}




// <rdar://problem/22333281> QoI: improve diagnostic when contextual type of closure disagrees with arguments
var _: () -> Int = {0}

// expected-error @+1 {{contextual type for closure argument list expects 1 argument, which cannot be implicitly ignored}} {{24-24=_ in }}
var _: (Int) -> Int = {0}

// expected-error @+1 {{contextual type for closure argument list expects 1 argument, which cannot be implicitly ignored}} {{24-24= _ in}}
var _: (Int) -> Int = { 0 }

// expected-error @+1 {{contextual type for closure argument list expects 2 arguments, which cannot be implicitly ignored}} {{29-29=_,_ in }}
var _: (Int, Int) -> Int = {0}

// expected-error @+1 {{contextual closure type '(Int, Int) -> Int' expects 2 arguments, but 3 were used in closure body}}
var _: (Int,Int) -> Int = {$0+$1+$2}

// expected-error @+1 {{contextual closure type '(Int, Int, Int) -> Int' expects 3 arguments, but 2 were used in closure body}}
var _: (Int, Int, Int) -> Int = {$0+$1}


// expected-error @+1 {{contextual closure type '(Int) -> Int' expects 1 argument, but 2 were used in closure body}}
var _: (Int) -> Int = {a,b in 0}

// expected-error @+1 {{contextual closure type '(Int) -> Int' expects 1 argument, but 3 were used in closure body}}
var _: (Int) -> Int = {a,b,c in 0}

// expected-error @+1 {{contextual closure type '(Int, Int, Int) -> Int' expects 3 arguments, but 2 were used in closure body}}
var _: (Int, Int, Int) -> Int = {a, b in a+b}

// <rdar://problem/15998821> Fail to infer types for closure that takes an inout argument
func r15998821() {
  func take_closure(_ x : (inout Int) -> ()) { }

  func test1() {
    take_closure { (a : inout Int) in
      a = 42
    }
  }

  func test2() {
    take_closure { a in
      a = 42
    }
  }

  func withPtr(_ body: (inout Int) -> Int) {}
  func f() { withPtr { p in return p } }

  let g = { x in x = 3 }
  take_closure(g)
}

// <rdar://problem/22602657> better diagnostics for closures w/o "in" clause
var _: (Int,Int) -> Int = {$0+$1+$2}  // expected-error {{contextual closure type '(Int, Int) -> Int' expects 2 arguments, but 3 were used in closure body}}


// Crash when re-typechecking bodies of non-single expression closures

struct CC {}
func callCC<U>(_ f: (CC) -> U) -> () {} // expected-note {{in call to function 'callCC'}}

func typeCheckMultiStmtClosureCrash() {
  callCC { // expected-error {{generic parameter 'U' could not be inferred}}
    _ = $0
    return 1
  }
}

// SR-832 - both these should be ok
func someFunc(_ foo: ((String) -> String)?,
              bar: @escaping (String) -> String) {
    let _: (String) -> String = foo != nil ? foo! : bar
    let _: (String) -> String = foo ?? bar
}

func verify_NotAC_to_AC_failure(_ arg: () -> ()) {
  func takesAC(_ arg: @autoclosure () -> ()) {}
  takesAC(arg) // expected-error {{add () to forward @autoclosure parameter}} {{14-14=()}}
}

// SR-1069 - Error diagnostic refers to wrong argument
class SR1069_W<T> {
  func append<Key: AnyObject>(value: T, forKey key: Key) where Key: Hashable {}
  // expected-note@-1 {{where 'Key' = 'Object?'}}
}
class SR1069_C<T> { let w: SR1069_W<(AnyObject, T) -> ()> = SR1069_W() }
struct S<T> {
  let cs: [SR1069_C<T>] = []

  func subscribe<Object: AnyObject>(object: Object?, method: (Object, T) -> ()) where Object: Hashable {
    let wrappedMethod = { (object: AnyObject, value: T) in }
    // expected-error @+2 {{instance method 'append(value:forKey:)' requires that 'Object?' be a class type}}
    // expected-note @+1 {{wrapped type 'Object' satisfies this requirement}}
    cs.forEach { $0.w.append(value: wrappedMethod, forKey: object) }
  }
}

// Similar to SR1069 but with multiple generic arguments
func simplified1069() {
  class C {}
  struct S {
    func genericallyNonOptional<T: AnyObject>(_ a: T, _ b: T, _ c: T) { }
    // expected-note@-1 {{where 'T' = 'Optional<C>'}}

    func f(_ a: C?, _ b: C?, _ c: C) {
      genericallyNonOptional(a, b, c) // expected-error {{instance method 'genericallyNonOptional' requires that 'Optional<C>' be a class type}}
      // expected-note @-1 {{wrapped type 'C' satisfies this requirement}}
    }
  }
}

// Make sure we cannot infer an () argument from an empty parameter list.
func acceptNothingToInt (_: () -> Int) {}
func testAcceptNothingToInt(ac1: @autoclosure () -> Int) {
  acceptNothingToInt({ac1($0)}) // expected-error@:27 {{argument passed to call that takes no arguments}}
  // expected-error@-1{{contextual closure type '() -> Int' expects 0 arguments, but 1 was used in closure body}}
}

// <rdar://problem/23570873> QoI: Poor error calling map without being able to infer "U" (closure result inference)
struct Thing {
  init?() {}
}
// This throws a compiler error
let things = Thing().map { thing in  // expected-error {{generic parameter 'U' could not be inferred}}
  // Commenting out this makes it compile
  _ = thing
  return thing
}


// <rdar://problem/21675896> QoI: [Closure return type inference] Swift cannot find members for the result of inlined lambdas with branches
func r21675896(file : String) {
  let x: String = { // expected-error {{unable to infer complex closure return type; add explicit type to disambiguate}} {{20-20= () -> String in }}
    if true {
      return "foo"
    }
    else {
      return file
    }
  }().pathExtension
}




// <rdar://problem/19870975> Incorrect diagnostic for failed member lookups within closures passed as arguments ("(_) -> _")
func ident<T>(_ t: T) -> T {}
var c = ident({1.DOESNT_EXIST}) // error: expected-error {{value of type 'Int' has no member 'DOESNT_EXIST'}}

// <rdar://problem/20712541> QoI: Int/UInt mismatch produces useless error inside a block
var afterMessageCount : Int?

func uintFunc() -> UInt {}
func takeVoidVoidFn(_ a : () -> ()) {}
takeVoidVoidFn { () -> Void in
  afterMessageCount = uintFunc()  // expected-error {{cannot assign value of type 'UInt' to type 'Int'}}
}

// <rdar://problem/19997471> Swift: Incorrect compile error when calling a function inside a closure
func f19997471(_ x: String) {} // expected-note {{candidate expects value of type 'String' for parameter #1}}
func f19997471(_ x: Int) {}    // expected-note {{candidate expects value of type 'Int' for parameter #1}}

func someGeneric19997471<T>(_ x: T) {
  takeVoidVoidFn {
    f19997471(x) // expected-error {{no exact matches in call to global function 'f19997471'}}
  }
}


// <rdar://problem/20921068> Swift fails to compile: [0].map() { _ in let r = (1,2).0; return r }
[0].map {  // expected-error {{generic parameter 'T' could not be inferred}}
  _ in
  let r =  (1,2).0
  return r
}


// <rdar://problem/21078316> Less than useful error message when using map on optional dictionary type
func rdar21078316() {
  var foo : [String : String]?
  var bar : [(String, String)]?
  bar = foo.map { ($0, $1) }  // expected-error {{contextual closure type '([String : String]) throws -> U' expects 1 argument, but 2 were used in closure body}}
}


// <rdar://problem/20978044> QoI: Poor diagnostic when using an incorrect tuple element in a closure
var numbers = [1, 2, 3]
zip(numbers, numbers).filter { $0.2 > 1 }  // expected-error {{value of tuple type '(Int, Int)' has no member '2'}}



// <rdar://problem/20868864> QoI: Cannot invoke 'function' with an argument list of type 'type'
func foo20868864(_ callback: ([String]) -> ()) { }
func rdar20868864(_ s: String) {
  var s = s
  foo20868864 { (strings: [String]) in
    s = strings   // expected-error {{cannot assign value of type '[String]' to type 'String'}}
  }
}

// <rdar://problem/22058555> crash in cs diags in withCString
func r22058555() {
  var firstChar: UInt8 = 0
  "abc".withCString { chars in
    firstChar = chars[0]  // expected-error {{cannot assign value of type 'Int8' to type 'UInt8'}} {{17-17=UInt8(}} {{25-25=)}}
  }
}

// <rdar://problem/20789423> Unclear diagnostic for multi-statement closure with no return type
func r20789423() {
  class C {
    func f(_ value: Int) { }
  }
  
  let p: C
  print(p.f(p)())  // expected-error {{cannot convert value of type 'C' to expected argument type 'Int'}}
  // expected-error@-1:11 {{cannot call value of non-function type '()'}}
  
  let _f = { (v: Int) in  // expected-error {{unable to infer complex closure return type; add explicit type to disambiguate}} {{23-23=-> String }}
    print("a")
    return "hi"
  }
  
}

// In the example below, SR-2505 started preferring C_SR_2505.test(_:) over
// test(it:). Prior to Swift 5.1, we emulated the old behavior. However,
// that behavior is inconsistent with the typical approach of preferring
// overloads from the concrete type over one from a protocol, so we removed
// the hack.
protocol SR_2505_Initable { init() }
struct SR_2505_II : SR_2505_Initable {}

protocol P_SR_2505 {
  associatedtype T: SR_2505_Initable
}

extension P_SR_2505 {
  func test(it o: (T) -> Bool) -> Bool {
    return o(T.self())
  }
}

class C_SR_2505 : P_SR_2505 {
  typealias T = SR_2505_II

  func test(_ o: Any) -> Bool {
    return false
  }

  func call(_ c: C_SR_2505) -> Bool {
    // Note: the diagnostic about capturing 'self', indicates that we have
    // selected test(_) rather than test(it:)
    return c.test { o in test(o) } // expected-error{{call to method 'test' in closure requires explicit 'self.' to make capture semantics explicit}}
  }
}

let _ = C_SR_2505().call(C_SR_2505())

// <rdar://problem/28909024> Returning incorrect result type from method invocation can result in nonsense diagnostic
extension Collection {
  func r28909024(_ predicate: (Iterator.Element)->Bool) -> Index {
    return startIndex
  }
}
func fn_r28909024(n: Int) {
  return (0..<10).r28909024 { // expected-error {{unexpected non-void return value in void function}}
    _ in true
  }
}

// SR-2994: Unexpected ambiguous expression in closure with generics
struct S_2994 {
  var dataOffset: Int
}
class C_2994<R> {
  init(arg: (R) -> Void) {}
}
func f_2994(arg: String) {}
func g_2994(arg: Int) -> Double {
  return 2
}
C_2994<S_2994>(arg: { (r: S_2994) in f_2994(arg: g_2994(arg: r.dataOffset)) }) // expected-error {{cannot convert value of type 'Double' to expected argument type 'String'}}

let _ = { $0[$1] }(1, 1) // expected-error {{value of type 'Int' has no subscripts}}
let _ = { $0 = ($0 = {}) } // expected-error {{assigning a variable to itself}}
let _ = { $0 = $0 = 42 } // expected-error {{assigning a variable to itself}}

// https://bugs.swift.org/browse/SR-403
// The () -> T => () -> () implicit conversion was kicking in anywhere
// inside a closure result, not just at the top-level.
let mismatchInClosureResultType : (String) -> ((Int) -> Void) = {
  (String) -> ((Int) -> Void) in
    return { }
    // expected-error@-1 {{contextual type for closure argument list expects 1 argument, which cannot be implicitly ignored}}
}

// SR-3520: Generic function taking closure with inout parameter can result in a variety of compiler errors or EXC_BAD_ACCESS
func sr3520_1<T>(_ g: (inout T) -> Int) {}
sr3520_1 { $0 = 1 } // expected-error {{cannot convert value of type '()' to closure result type 'Int'}}

// This test makes sure that having closure with inout argument doesn't crash with member lookup
struct S_3520 {
  var number1: Int
}
func sr3520_set_via_closure<S, T>(_ closure: (inout S, T) -> ()) {} // expected-note {{in call to function 'sr3520_set_via_closure'}}
sr3520_set_via_closure({ $0.number1 = $1 })
// expected-error@-1 {{generic parameter 'S' could not be inferred}}
// expected-error@-2 {{generic parameter 'T' could not be inferred}}

// SR-3073: UnresolvedDotExpr in single expression closure

struct SR3073Lense<Whole, Part> {
  let set: (inout Whole, Part) -> ()
}
struct SR3073 {
  var number1: Int
  func lenses() {
    let _: SR3073Lense<SR3073, Int> = SR3073Lense(
      set: { $0.number1 = $1 } // ok
    )
  }
}

// SR-3479: Segmentation fault and other error for closure with inout parameter
func sr3497_unfold<A, B>(_ a0: A, next: (inout A) -> B) {}
func sr3497() {
  let _ = sr3497_unfold((0, 0)) { s in 0 } // ok
}

// SR-3758: Swift 3.1 fails to compile 3.0 code involving closures and IUOs
let _: ((Any?) -> Void) = { (arg: Any!) in }
// This example was rejected in 3.0 as well, but accepting it is correct.
let _: ((Int?) -> Void) = { (arg: Int!) in }

// rdar://30429709 - We should not attempt an implicit conversion from
// () -> T to () -> Optional<()>.
func returnsArray() -> [Int] { return [] }

returnsArray().compactMap { $0 }.compactMap { }
// expected-warning@-1 {{expression of type 'Int' is unused}}
// expected-warning@-2 {{result of call to 'compactMap' is unused}}

// rdar://problem/30271695
_ = ["hi"].compactMap { $0.isEmpty ? nil : $0 }

// rdar://problem/32432145 - compiler should emit fixit to remove "_ in" in closures if 0 parameters is expected

func r32432145(_ a: () -> ()) {}

r32432145 { _ in let _ = 42 }
// expected-error@-1 {{contextual closure type '() -> ()' expects 0 arguments, but 1 was used in closure body}} {{13-17=}}

r32432145 { _ in
  // expected-error@-1 {{contextual closure type '() -> ()' expects 0 arguments, but 1 was used in closure body}} {{13-17=}}
  print("answer is 42")
}

r32432145 { _,_ in
  // expected-error@-1 {{contextual closure type '() -> ()' expects 0 arguments, but 2 were used in closure body}} {{13-19=}}
  print("answer is 42")
}

// rdar://problem/30106822 - Swift ignores type error in closure and presents a bogus error about the caller
[1, 2].first { $0.foo = 3 }
// expected-error@-1 {{value of type 'Int' has no member 'foo'}}
// expected-error@-2 {{cannot convert value of type '()' to closure result type 'Bool'}}

// rdar://problem/32433193, SR-5030 - Higher-order function diagnostic mentions the wrong contextual type conversion problem
protocol A_SR_5030 {
  associatedtype Value
  func map<U>(_ t : @escaping (Self.Value) -> U) -> B_SR_5030<U>
}

struct B_SR_5030<T> : A_SR_5030 {
  typealias Value = T
  func map<U>(_ t : @escaping (T) -> U) -> B_SR_5030<U> { fatalError() }
}

func sr5030_exFalso<T>() -> T {
  fatalError()
}

extension A_SR_5030 {
  func foo() -> B_SR_5030<Int> {
    let tt : B_SR_5030<Int> = sr5030_exFalso()
    return tt.map { x in (idx: x) }
    // expected-error@-1 {{cannot convert value of type '(idx: Int)' to closure result type 'Int'}}
  }
}

// rdar://problem/33296619
let u = rdar33296619().element //expected-error {{use of unresolved identifier 'rdar33296619'}}

[1].forEach { _ in
  _ = "\(u)"
  _ = 1 + "hi" // expected-error {{binary operator '+' cannot be applied to operands of type 'Int' and 'String'}}
  // expected-note@-1 {{overloads for '+' exist with these partially matching parameter lists: (Int, Int), (String, String)}}
}

class SR5666 {
  var property: String?
}

func testSR5666(cs: [SR5666?]) -> [String?] {
  return cs.map({ c in
      let a = c.propertyWithTypo ?? "default"
      // expected-error@-1 {{value of type 'SR5666?' has no member 'propertyWithTypo'}}
      let b = "\(a)"
      return b
    })
}

// Ensure that we still do the appropriate pointer conversion here.
_ = "".withCString { UnsafeMutableRawPointer(mutating: $0) }

// rdar://problem/34077439 - Crash when pre-checking bails out and
// leaves us with unfolded SequenceExprs inside closure body.
_ = { (offset) -> T in // expected-error {{use of undeclared type 'T'}}
  return offset ? 0 : 0
}

struct SR5202<T> {
  func map<R>(fn: (T) -> R) {}
}

SR5202<()>().map{ return 0 }
SR5202<()>().map{ _ in return 0 }
SR5202<Void>().map{ return 0 }
SR5202<Void>().map{ _ in return 0 }

func sr3520_2<T>(_ item: T, _ update: (inout T) -> Void) {
  var x = item
  update(&x)
}
var sr3250_arg = 42
sr3520_2(sr3250_arg) { $0 += 3 } // ok

// SR-1976/SR-3073: Inference of inout
func sr1976<T>(_ closure: (inout T) -> Void) {}
sr1976({ $0 += 2 }) // ok

// rdar://problem/33429010

struct I_33429010 : IteratorProtocol {
    func next() -> Int? {
        fatalError()
    }
}

extension Sequence {
    public func rdar33429010<Result>(into initialResult: Result,
                                     _ nextPartialResult: (_ partialResult: inout Result, Iterator.Element) throws -> ()
        ) rethrows -> Result {
        return initialResult
    }
}

extension Int {
   public mutating func rdar33429010_incr(_ inc: Int) {
     self += inc
   }
}

func rdar33429010_2() {
  let iter = I_33429010()
  var acc: Int = 0 // expected-warning {{}}
  let _: Int = AnySequence { iter }.rdar33429010(into: acc, { $0 + $1 })
  // expected-warning@-1 {{result of operator '+' is unused}}
  let _: Int = AnySequence { iter }.rdar33429010(into: acc, { $0.rdar33429010_incr($1) })
}

class P_33429010 {
  var name: String = "foo"
}

class C_33429010 : P_33429010 {
}

func rdar33429010_3() {
 let arr = [C_33429010()]
 let _ = arr.map({ ($0.name, $0 as P_33429010) }) // Ok
}

func rdar36054961() {
  func bar(dict: [String: (inout String, Range<String.Index>, String) -> Void]) {}
  bar(dict: ["abc": { str, range, _ in
     str.replaceSubrange(range, with: str[range].reversed())
  }])
}

protocol P_37790062 {
  associatedtype T
  var elt: T { get }
}

func rdar37790062() {
  struct S<T> {
    init(_ a: () -> T, _ b: () -> T) {}
  }

  class C1 : P_37790062 {
    typealias T = Int
    var elt: T { return 42 }
  }

  class C2 : P_37790062 {
    typealias T = (String, Int, Void)
    var elt: T { return ("question", 42, ()) }
  }

  func foo() -> Int { return 42 }
  func bar() -> Void {}
  func baz() -> (String, Int) { return ("question", 42) }
  func bzz<T>(_ a: T) -> T { return a }
  func faz<T: P_37790062>(_ a: T) -> T.T { return a.elt }

  _ = S({ foo() }, { bar() }) // expected-warning {{result of call to 'foo()' is unused}}
  _ = S({ baz() }, { bar() }) // expected-warning {{result of call to 'baz()' is unused}}
  _ = S({ bzz(("question", 42)) }, { bar() }) // expected-warning {{result of call to 'bzz' is unused}}
  _ = S({ bzz(String.self) }, { bar() }) // expected-warning {{result of call to 'bzz' is unused}}
  _ = S({ bzz(((), (()))) }, { bar() }) // expected-warning {{result of call to 'bzz' is unused}}
  _ = S({ bzz(C1()) }, { bar() }) // expected-warning {{result of call to 'bzz' is unused}}
  _ = S({ faz(C2()) }, { bar() }) // expected-warning {{result of call to 'faz' is unused}}
}

// <rdar://problem/39489003>
typealias KeyedItem<K, T> = (key: K, value: T) // expected-note {{'T' declared as parameter to type 'KeyedItem'}}

protocol Node {
  associatedtype T
  associatedtype E
  associatedtype K
  var item: E {get set}
  var children: [(key: K, value: T)] {get set}
}

extension Node {
  func getChild(for key:K)->(key: K, value: T) {
    return children.first(where: { (item:KeyedItem) -> Bool in
        return item.key == key
        // expected-error@-1 {{generic parameter 'T' could not be inferred}}
      })!
  }
}

// Make sure we don't allow this anymore
func takesTwo(_: (Int, Int) -> ()) {}
func takesTwoInOut(_: (Int, inout Int) -> ()) {}

takesTwo { _ in } // expected-error {{contextual closure type '(Int, Int) -> ()' expects 2 arguments, but 1 was used in closure body}}
takesTwoInOut { _ in } // expected-error {{contextual closure type '(Int, inout Int) -> ()' expects 2 arguments, but 1 was used in closure body}}

// <rdar://problem/20371273> Type errors inside anonymous functions don't provide enough information
func f20371273() {
  let x: [Int] = [1, 2, 3, 4]
  let y: UInt = 4
  _ = x.filter { ($0 + y)  > 42 } // expected-error {{cannot convert value of type 'UInt' to expected argument type 'Int'}}
}

// rdar://problem/42337247

func overloaded(_ handler: () -> Int) {} // expected-note {{found this candidate}}
func overloaded(_ handler: () -> Void) {} // expected-note {{found this candidate}}

overloaded { } // empty body => inferred as returning ()

overloaded { print("hi") } // single-expression closure => typechecked with body

overloaded { print("hi"); print("bye") } // multiple expression closure without explicit returns; can default to any return type
// expected-error@-1 {{ambiguous use of 'overloaded'}}

func not_overloaded(_ handler: () -> Int) {}

not_overloaded { } // empty body
// expected-error@-1 {{cannot convert value of type '()' to closure result type 'Int'}}

not_overloaded { print("hi") } // single-expression closure
// expected-error@-1 {{cannot convert value of type '()' to closure result type 'Int'}}

// no error in -typecheck, but dataflow diagnostics will complain about missing return
not_overloaded { print("hi"); print("bye") } // multiple expression closure

func apply(_ fn: (Int) throws -> Int) rethrows -> Int {
  return try fn(0)
}

enum E : Error {
  case E
}

func test() -> Int? {
  return try? apply({ _ in throw E.E })
}

var fn: () -> [Int] = {}
// expected-error@-1 {{cannot convert value of type '()' to closure result type '[Int]'}}

fn = {}
// expected-error@-1 {{cannot assign value of type '() -> ()' to type '() -> [Int]'}}

func test<Instances : Collection>(
  _ instances: Instances,
  _ fn: (Instances.Index, Instances.Index) -> Bool
) { fatalError() }

test([1]) { _, _ in fatalError(); () }

// rdar://problem/40537960 - Misleading diagnostic when using closure with wrong type

protocol P_40537960 {}
func rdar_40537960() {
  struct S {
    var v: String
  }

  struct L : P_40537960 {
    init(_: String) {}
  }

  struct R<T : P_40537960> {
    init(_: P_40537960) {}
  }

  struct A<T: Collection, P: P_40537960> { // expected-note {{'P' declared as parameter to type 'A'}}
    typealias Data = T.Element
    init(_: T, fn: (Data) -> R<P>) {}
  }

  var arr: [S] = []
  _ = A(arr, fn: { L($0.v) }) // expected-error {{cannot convert value of type 'L' to closure result type 'R<P>'}}
  // expected-error@-1 {{generic parameter 'P' could not be inferred}}
  // expected-note@-2 {{explicitly specify the generic arguments to fix this issue}} {{8-8=<[S], <#P: P_40537960#>>}}
}

// rdar://problem/45659733
func rdar_45659733() {
  func foo<T : BinaryInteger>(_: AnyHashable, _: T) {}
  func bar(_ a: Int, _ b: Int) {
    _ = (a ..< b).map { i in foo(i, i) } // Ok
  }

  struct S<V> {
    func map<T>(
      get: @escaping (V) -> T,
      set: @escaping (inout V, T) -> Void
    ) -> S<T> {
      fatalError()
    }

    subscript<T>(
      keyPath: WritableKeyPath<V, T?>,
      default defaultValue: T
    ) -> S<T> {
      return map(
        get: { $0[keyPath: keyPath] ?? defaultValue },
        set: { $0[keyPath: keyPath] = $1 }
      ) // Ok, make sure that we deduce result to be S<T>
    }
  }
}

func rdar45771997() {
  struct S {
    mutating func foo() {}
  }

  let _: Int = { (s: inout S) in s.foo() }
  // expected-error@-1 {{cannot convert value of type '(inout S) -> ()' to specified type 'Int'}}
}

struct rdar30347997 {
  func withUnsafeMutableBufferPointer(body : (inout Int) -> ()) {}
  func foo() {
    withUnsafeMutableBufferPointer { // expected-error {{cannot convert value of type '(Int) -> ()' to expected argument type '(inout Int) -> ()'}}
      (b : Int) in
    }
  }
}

struct rdar43866352<Options> {
  func foo() {
    let callback: (inout Options) -> Void
    callback = { (options: Options) in } // expected-error {{cannot assign value of type '(Options) -> ()' to type '(inout Options) -> Void'}}
  }
}

extension Hashable {
  var self_: Self {
    return self
  }
}

do {
  struct S<
      C : Collection,
      I : Hashable,
      R : Numeric
  > {
    init(_ arr: C,
         id: KeyPath<C.Element, I>,
         content: @escaping (C.Element) -> R) {}
  }

  func foo(_ arr: [Int]) {
    _ = S(arr, id: \.self_) {
      // expected-error@-1 {{contextual type for closure argument list expects 1 argument, which cannot be implicitly ignored}} {{30-30=_ in }}
      return 42
    }
  }
}

// Don't allow result type of a closure to end up as a noescape type

// The funny error is because we infer the type of badResult as () -> ()
// via the 'T -> U => T -> ()' implicit conversion.
let badResult = { (fn: () -> ()) in fn }
// expected-error@-1 {{expression resolves to an unused function}}

// rdar://problem/55102498 - closure's result type can't be inferred if the last parameter has a default value
func test_trailing_closure_with_defaulted_last() {
  func foo<T>(fn: () -> T, value: Int = 0) {}
  foo { 42 } // Ok
  foo(fn: { 42 }) // Ok
}
