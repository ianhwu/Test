// RUN: %target-swift-frontend -emit-sil -verify %s | %FileCheck %s

struct Point {
  let x: Int
  var y: Int
}

struct Rectangle {
  var topLeft, bottomRight: Point
}

@dynamicMemberLookup
struct Lens<T> {
  var obj: T

  init(_ obj: T) {
    self.obj = obj
  }

  subscript<U>(dynamicMember member: KeyPath<T, U>) -> Lens<U> {
    get { return Lens<U>(obj[keyPath: member]) }
  }

  subscript<U>(dynamicMember member: WritableKeyPath<T, U>) -> Lens<U> {
    get { return Lens<U>(obj[keyPath: member]) }
    set { obj[keyPath: member] = newValue.obj }
  }

  // Used to make sure that keypath and string based lookup are
  // property disambiguated.
  subscript(dynamicMember member: String) -> Lens<Int> {
    return Lens<Int>(42)
  }
}

var topLeft = Point(x: 0, y: 0)
var bottomRight = Point(x: 10, y: 10)

var lens = Lens(Rectangle(topLeft: topLeft,
                          bottomRight: bottomRight))

// CHECK: function_ref @$s29keypath_dynamic_member_lookup4LensV0B6MemberACyqd__Gs15WritableKeyPathCyxqd__G_tcluig
// CHECK-NEXT: apply %45<Rectangle, Point>({{.*}})
// CHECK: function_ref @$s29keypath_dynamic_member_lookup4LensV0B6MemberACyqd__Gs7KeyPathCyxqd__G_tcluig
// CHECK-NEXT: apply %54<Point, Int>({{.*}})
_ = lens.topLeft.x

// CHECK: function_ref @$s29keypath_dynamic_member_lookup4LensV0B6MemberACyqd__Gs15WritableKeyPathCyxqd__G_tcluig
// CHECK-NEXT: apply %69<Rectangle, Point>({{.*}})
// CHECK: function_ref @$s29keypath_dynamic_member_lookup4LensV0B6MemberACyqd__Gs15WritableKeyPathCyxqd__G_tcluig
// CHECK-NEXT: apply %76<Point, Int>({{.*}})
_ = lens.topLeft.y

lens.topLeft = Lens(Point(x: 1, y: 2)) // Ok
lens.bottomRight.y = Lens(12)          // Ok

@dynamicMemberLookup
class A<T> {
  var value: T

  init(_ v: T) {
    self.value = v
  }

  subscript<U>(dynamicMember member: KeyPath<T, U>) -> U {
    get { return value[keyPath: member] }
  }
}

// Let's make sure that keypath dynamic member lookup
// works with inheritance

class B<T> : A<T> {}

func bar(_ b: B<Point>) {
  let _: Int = b.x
  let _ = b.y
}

struct Point3D {
  var x, y, z: Int
}

// Make sure that explicitly declared members take precedence
class C<T> : A<T> {
  var x: Float = 42
}

func baz(_ c: C<Point3D>) {
  // CHECK: ref_element_addr {{.*}} : $C<Point3D>, #C.x
  let _ = c.x
  // CHECK: [[Y:%.*]] = keypath $KeyPath<Point3D, Int>, (root $Point3D; stored_property #Point3D.z : $Int)
  // CHECK: [[KEYPATH:%.*]] = function_ref @$s29keypath_dynamic_member_lookup1AC0B6Memberqd__s7KeyPathCyxqd__G_tcluig
  // CHECK-NEXT: apply [[KEYPATH]]<Point3D, Int>({{.*}}, [[Y]], {{.*}})
  let _ = c.z
}

@dynamicMemberLookup
struct SubscriptLens<T> {
  var value: T

  subscript(foo: String) -> Int {
    get { return 42 }
  }

  subscript<U>(dynamicMember member: KeyPath<T, U>) -> U {
    get { return value[keyPath: member] }
  }

  subscript<U>(dynamicMember member: WritableKeyPath<T, U>) -> U {
    get { return value[keyPath: member] }
    set { value[keyPath: member] = newValue }
  }
}

func keypath_with_subscripts(_ arr: SubscriptLens<[Int]>,
                             _ dict: inout SubscriptLens<[String: Int]>) {
  // CHECK: keypath $WritableKeyPath<Array<Int>, ArraySlice<Int>>, (root $Array<Int>; settable_property $ArraySlice<Int>,  id @$sSays10ArraySliceVyxGSnySiGcig : {{.*}})
  _ = arr[0..<3]
  // CHECK: keypath $KeyPath<Array<Int>, Int>, (root $Array<Int>; gettable_property $Int,  id @$sSa5countSivg : {{.*}})
  for idx in 0..<arr.count {
    // CHECK: keypath $WritableKeyPath<Array<Int>, Int>, (root $Array<Int>; settable_property $Int,  id @$sSayxSicig : {{.*}})
    let _ = arr[idx]
    // CHECK: keypath $WritableKeyPath<Array<Int>, Int>, (root $Array<Int>; settable_property $Int,  id @$sSayxSicig : {{.*}})
    print(arr[idx])
  }

  // CHECK: function_ref @$s29keypath_dynamic_member_lookup13SubscriptLensVySiSScig
  _ = arr["hello"]
  // CHECK: function_ref @$s29keypath_dynamic_member_lookup13SubscriptLensVySiSScig
  _ = dict["hello"]

  if let index = dict.value.firstIndex(where: { $0.value == 42 }) {
    // CHECK: keypath $KeyPath<Dictionary<String, Int>, (key: String, value: Int)>, (root $Dictionary<String, Int>; gettable_property $(key: String, value: Int),  id @$sSDyx3key_q_5valuetSD5IndexVyxq__Gcig : {{.*}})
    let _ = dict[index]
  }
  // CHECK: keypath $WritableKeyPath<Dictionary<String, Int>, Optional<Int>>, (root $Dictionary<String, Int>; settable_property $Optional<Int>,  id @$sSDyq_Sgxcig : {{.*}})
  dict["ultimate question"] = 42
}

struct DotStruct {
  var x, y: Int
}

class DotClass {
  var x, y: Int

  init(x: Int, y: Int) {
    self.x = x
    self.y = y
  }
}

@dynamicMemberLookup
struct DotLens<T> {
  var value: T

  subscript<U>(dynamicMember member: WritableKeyPath<T, U>) -> U {
    get { return value[keyPath: member] }
    set { value[keyPath: member] = newValue }
  }

  subscript<U>(dynamicMember member: ReferenceWritableKeyPath<T, U>) -> U {
    get { return value[keyPath: member] }
    set { value[keyPath: member] = newValue }
  }
}

func dot_struct_test(_ lens: inout DotLens<DotStruct>) {
  // CHECK: keypath $WritableKeyPath<DotStruct, Int>, (root $DotStruct; stored_property #DotStruct.x : $Int)
  lens.x = 1
  // CHECK: keypath $WritableKeyPath<DotStruct, Int>, (root $DotStruct; stored_property #DotStruct.y : $Int)
  let _ = lens.y
}

func dot_class_test(_ lens: inout DotLens<DotClass>) {
  // CHECK: keypath $ReferenceWritableKeyPath<DotClass, Int>, (root $DotClass; settable_property $Int,  id #DotClass.x!getter.1 : (DotClass) -> () -> Int, getter @$s29keypath_dynamic_member_lookup8DotClassC1xSivpACTK : {{.*}})
  lens.x = 1
  // CHECK: keypath $ReferenceWritableKeyPath<DotClass, Int>, (root $DotClass; settable_property $Int,  id #DotClass.y!getter.1 : (DotClass) -> () -> Int, getter @$s29keypath_dynamic_member_lookup8DotClassC1ySivpACTK : {{.*}})
  let _ = lens.y
}

@dynamicMemberLookup
struct OverloadedLens<T> {
  var value: T

  subscript<U>(keyPath: KeyPath<T, U>) -> U {
    get { return value[keyPath: keyPath] }
  }

  subscript<U>(dynamicMember keyPath: KeyPath<T, U>) -> U {
    get { return value[keyPath: keyPath] }
  }
}

// Make sure if there is a subscript which accepts key path,
// existing dynamic member overloads wouldn't interfere.
func test_direct_subscript_ref(_ lens: OverloadedLens<Point>) {
  // CHECK: function_ref @$s29keypath_dynamic_member_lookup14OverloadedLensVyqd__s7KeyPathCyxqd__Gcluig
  _ = lens[\.x]
  // CHECK: function_ref @$s29keypath_dynamic_member_lookup14OverloadedLensVyqd__s7KeyPathCyxqd__Gcluig
  _ = lens[\.y]

  // CHECK: function_ref @$s29keypath_dynamic_member_lookup14OverloadedLensV0B6Memberqd__s7KeyPathCyxqd__G_tcluig
  _ = lens.x
  // CHECK: function_ref @$s29keypath_dynamic_member_lookup14OverloadedLensV0B6Memberqd__s7KeyPathCyxqd__G_tcluig
  _ = lens.y
}

func test_keypath_dynamic_lookup_inside_keypath() {
  // CHECK: keypath $KeyPath<Point, Int>, (root $Point; stored_property #Point.x : $Int)
  // CHECK-NEXT: keypath $KeyPath<Lens<Point>, Lens<Int>>, (root $Lens<Point>; gettable_property $Lens<Int>,  id @$s29keypath_dynamic_member_lookup4LensV0B6MemberACyqd__Gs7KeyPathCyxqd__G_tcluig : {{.*}})
  _ = \Lens<Point>.x
  // CHECK: keypath $WritableKeyPath<Rectangle, Point>, (root $Rectangle; stored_property #Rectangle.topLeft : $Point)
  // CHECK-NEXT: keypath $WritableKeyPath<Point, Int>, (root $Point; stored_property #Point.y : $Int)
  // CHECK-NEXT: keypath $WritableKeyPath<Lens<Rectangle>, Lens<Int>>, (root $Lens<Rectangle>; settable_property $Lens<Point>,  id @$s29keypath_dynamic_member_lookup4LensV0B6MemberACyqd__Gs15WritableKeyPathCyxqd__G_tcluig : {{.*}})
  _ = \Lens<Rectangle>.topLeft.y
  // CHECK: keypath $KeyPath<Array<Int>, Int>, (root $Array<Int>; gettable_property $Int,  id @$sSa5countSivg : {{.*}})
  // CHECK-NEXT: keypath $KeyPath<Lens<Array<Int>>, Lens<Int>>, (root $Lens<Array<Int>>; gettable_property $Lens<Int>,  id @$s29keypath_dynamic_member_lookup4LensV0B6MemberACyqd__Gs7KeyPathCyxqd__G_tcluig : {{.*}})
  _ = \Lens<[Int]>.count
  // CHECK: keypath $WritableKeyPath<Array<Int>, Int>, (root $Array<Int>; settable_property $Int,  id @$sSayxSicig : {{.*}})
  // CHECK-NEXT: keypath $WritableKeyPath<Lens<Array<Int>>, Lens<Int>>, (root $Lens<Array<Int>>; settable_property $Lens<Int>,  id @$s29keypath_dynamic_member_lookup4LensV0B6MemberACyqd__Gs15WritableKeyPathCyxqd__G_tcluig : {{.*}})
  _ = \Lens<[Int]>.[0]
  // CHECK: keypath $WritableKeyPath<Array<Array<Int>>, Array<Int>>, (root $Array<Array<Int>>; settable_property $Array<Int>,  id @$sSayxSicig : {{.*}})
  // CHECK-NEXT: keypath $KeyPath<Array<Int>, Int>, (root $Array<Int>; gettable_property $Int,  id @$sSa5countSivg : {{.*}})
  // CHECK-NEXT: keypath $KeyPath<Lens<Array<Array<Int>>>, Lens<Int>>, (root $Lens<Array<Array<Int>>>; settable_property $Lens<Array<Int>>,  id @$s29keypath_dynamic_member_lookup4LensV0B6MemberACyqd__Gs15WritableKeyPathCyxqd__G_tcluig : {{.*}})
  _ = \Lens<[[Int]]>.[0].count
}

func test_recursive_dynamic_lookup(_ lens: Lens<Lens<Point>>) {
  // CHECK: keypath $KeyPath<Point, Int>, (root $Point; stored_property #Point.x : $Int)
  // CHECK-NEXT: keypath $KeyPath<Lens<Point>, Lens<Int>>, (root $Lens<Point>; gettable_property $Lens<Int>,  id @$s29keypath_dynamic_member_lookup4LensV0B6MemberACyqd__Gs7KeyPathCyxqd__G_tcluig : {{.*}})
  // CHECK: function_ref @$s29keypath_dynamic_member_lookup4LensV0B6MemberACyqd__Gs7KeyPathCyxqd__G_tcluig
  _ = lens.x
  // CHECK: keypath $KeyPath<Point, Int>, (root $Point; stored_property #Point.x : $Int)
  // CHECK: function_ref @$s29keypath_dynamic_member_lookup4LensV0B6MemberACyqd__Gs7KeyPathCyxqd__G_tcluig
  _ = lens.obj.x
  // CHECK: [[FIRST_OBJ:%.*]] = struct_extract {{.*}} : $Lens<Lens<Point>>, #Lens.obj
  // CHECK-NEXT: [[SECOND_OBJ:%.*]] = struct_extract [[FIRST_OBJ]] : $Lens<Point>, #Lens.obj
  // CHECK-NEXT: struct_extract [[SECOND_OBJ]] : $Point, #Point.y
  _ = lens.obj.obj.y
  // CHECK: keypath $KeyPath<Point, Int>, (root $Point; stored_property #Point.x : $Int)
  // CHECK-NEXT: keypath $KeyPath<Lens<Point>, Lens<Int>>, (root $Lens<Point>; gettable_property $Lens<Int>,  id @$s29keypath_dynamic_member_lookup4LensV0B6MemberACyqd__Gs7KeyPathCyxqd__G_tcluig : {{.*}})
  // CHECK-NEXT: keypath $KeyPath<Lens<Lens<Point>>, Lens<Lens<Int>>>, (root $Lens<Lens<Point>>; gettable_property $Lens<Lens<Int>>,  id @$s29keypath_dynamic_member_lookup4LensV0B6MemberACyqd__Gs7KeyPathCyxqd__G_tcluig : {{.*}})
  _ = \Lens<Lens<Point>>.x
  // CHECK: keypath $WritableKeyPath<Rectangle, Point>, (root $Rectangle; stored_property #Rectangle.topLeft : $Point)
  // CHECK-NEXT: keypath $WritableKeyPath<Lens<Rectangle>, Lens<Point>>, (root $Lens<Rectangle>; settable_property $Lens<Point>,  id @$s29keypath_dynamic_member_lookup4LensV0B6MemberACyqd__Gs15WritableKeyPathCyxqd__G_tcluig : {{.*}})
  // CHECK-NEXT: keypath $KeyPath<Point, Int>, (root $Point; stored_property #Point.x : $Int)
  // CHECK-NEXT: keypath $KeyPath<Lens<Point>, Lens<Int>>, (root $Lens<Point>; gettable_property $Lens<Int>,  id @$s29keypath_dynamic_member_lookup4LensV0B6MemberACyqd__Gs7KeyPathCyxqd__G_tcluig : {{.*}})
  // CHECK-NEXT: keypath $KeyPath<Lens<Lens<Rectangle>>, Lens<Lens<Int>>>, (root $Lens<Lens<Rectangle>>; settable_property $Lens<Lens<Point>>,  id @$s29keypath_dynamic_member_lookup4LensV0B6MemberACyqd__Gs15WritableKeyPathCyxqd__G_tcluig : {{.*}})
  _ = \Lens<Lens<Rectangle>>.topLeft.x
}

@dynamicMemberLookup
struct RefWritableBox<T> {
  var obj: T

  init(_ obj: T) {
    self.obj = obj
  }

  subscript<U>(dynamicMember member: KeyPath<T, U>) -> U {
    get { return obj[keyPath: member] }
  }

  subscript<U>(dynamicMember member: ReferenceWritableKeyPath<T, U>) -> U {
    get { return obj[keyPath: member] }
    set { obj[keyPath: member] = newValue }
  }
}

func prefer_readonly_keypath_over_reference_writable() {
  class C {
    let foo: Int

    init(_ foo: Int) {
      self.foo = foo
    }
  }

  var box = RefWritableBox(C(42))
  // expected-warning@-1 {{variable 'box' was never mutated; consider changing to 'let' constant}}

  // CHECK: function_ref RefWritableBox.subscript.getter
  // CHECK-NEXT: function_ref @$s29keypath_dynamic_member_lookup14RefWritableBoxV0B6Memberqd__s7KeyPathCyxqd__G_tcluig
  _ = box.foo
}


// rdar://problem/52779809 - condiitional conformance shadows names of members reachable through dynamic lookup

protocol P {
  var foo: Int { get }
}

@dynamicMemberLookup struct Ref<T> {
  var value: T

  subscript<U>(dynamicMember member: KeyPath<T, U>) -> U {
    get { return value[keyPath: member] }
  }
}

extension P {
  var foo: Int { return 42 }
}

struct S {
  var foo: Int { return 0 }
  var baz: Int { return 1 }
}

struct Q {
  var bar: Int { return 1 }
}

extension Ref : P where T == Q {
  var baz: String { return "hello" }
}

func rdar52779809(_ ref1: Ref<S>, _ ref2: Ref<Q>) {
  // CHECK: function_ref @$s29keypath_dynamic_member_lookup3RefV0B6Memberqd__s7KeyPathCyxqd__G_tcluig
  _ = ref1.foo // Ok
  // CHECK: function_ref @$s29keypath_dynamic_member_lookup3RefV0B6Memberqd__s7KeyPathCyxqd__G_tcluig
  _ = ref1.baz // Ok
  // CHECK: function_ref @$s29keypath_dynamic_member_lookup1PPAAE3fooSivg
  _ = ref2.foo // Ok
  // CHECK: function_ref @$s29keypath_dynamic_member_lookup3RefV0B6Memberqd__s7KeyPathCyxqd__G_tcluig
  _ = ref2.bar // Ok
}

func make_sure_delayed_keypath_dynamic_member_works() {
  @propertyWrapper @dynamicMemberLookup
  struct Wrapper<T> {
    var storage: T? = nil

    var wrappedValue: T {
      get { storage! }
    }

    var projectedValue: Wrapper<T> { self }

    init() { }

    init(wrappedValue: T) {
      storage = wrappedValue
    }

    subscript<Property>(dynamicMember keyPath: KeyPath<T, Property>) -> Wrapper<Property> {
      get { .init() }
    }
  }

  struct Field {
    @Wrapper var v: Bool = true
  }

  struct Arr {
    var fields: [Field] = []
  }

  struct Test {
    @Wrapper var data: Arr

    func test(_ index: Int) {
      let _ = self.$data.fields[index].v.wrappedValue
    }
  }
}


// SR-11465 - Ambiguity in expression which matches both dynamic member lookup and declaration from constrained extension

@dynamicMemberLookup
struct SR_11465<RawValue> {
  var rawValue: RawValue

  subscript<Subject>(dynamicMember keyPath: KeyPath<RawValue, Subject>) -> Subject {
    rawValue[keyPath: keyPath]
  }
}

extension SR_11465: Hashable, Equatable where RawValue: Hashable {
  func hash(into hasher: inout Hasher) {
    hasher.combine(self.rawValue)
  }
}

func test_constrained_ext_vs_dynamic_member() {
  // CHECK: function_ref @$s29keypath_dynamic_member_lookup8SR_11465VAASHRzlE9hashValueSivg
  _ = SR_11465<Int>(rawValue: 1).hashValue // Ok, keep choice from constrained extension
}
