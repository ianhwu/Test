// RUN: %target-swift-frontend -primary-file %s -emit-ir | %FileCheck %s
// RUN: %target-swift-frontend -primary-file %s -O -emit-ir | %FileCheck %s --check-prefix=OPT

// REQUIRES: objc_interop

import Foundation

// We mustn't hoist the alloc_stack for measurement out of the availability
// guard.

// CHECK-LABEL: define{{.*}} @{{.*}}dontHoist
// CHECK-NOT: s10Foundation11MeasurementVySo17NSUnitTemperature
// CHECK: call swiftcc i1 @"$ss26_stdlib_isOSVersionAtLeastyBi1_Bw_BwBwtF"(
// CHECK: s10Foundation11MeasurementVySo17NSUnitTemperature

// OPT-LABEL: define{{.*}} @{{.*}}dontHoist
// OPT-NOT: S10Foundation11MeasurementVySo17NSUnitTemperature
// OPT: call {{.*}} @_swift_stdlib_operatingSystemVersion(
// OPT: s10Foundation11MeasurementVySo17NSUnitTemperature

public func dontHoist() {
  if #available(OSX 10.12, iOS 10.0, watchOS 3.0, tvOS 10.0, *) {
      let measurement = Measurement<UnitTemperature>(value: Double(42), unit: .celsius)
      print("\(measurement)")
  } else {
      print("Not measurement")
  }
}


// With optimizations on, multiple #availability checks should generate only
// a single call into _swift_stdlib_operatingSystemVersion.

// CHECK-LABEL: define{{.*}} @{{.*}}multipleAvailabilityChecks
// CHECK: call swiftcc i1 @"$ss26_stdlib_isOSVersionAtLeastyBi1_Bw_BwBwtF"(
// CHECK: call swiftcc i1 @"$ss26_stdlib_isOSVersionAtLeastyBi1_Bw_BwBwtF"(
// CHECK: call swiftcc i1 @"$ss26_stdlib_isOSVersionAtLeastyBi1_Bw_BwBwtF"(
// CHECK: ret void

// OPT-LABEL: define{{.*}} @{{.*}}multipleAvailabilityChecks
// OPT: call {{.*}} @_swift_stdlib_operatingSystemVersion
// OPT-NOT: call {{.*}} @_swift_stdlib_operatingSystemVersion
// OPT: ret void
public func multipleAvailabilityChecks() {
  if #available(OSX 10.12, iOS 10.0, watchOS 3.0, tvOS 10.0, *) {
    print("test one")
  }
  if #available(OSX 10.11, iOS 9.0, watchOS 2.0, tvOS 9.0, *) {
    print("test two")
  }
  if #available(OSX 10.10, iOS 8.0, watchOS 1.0, tvOS 8.0, *) {
    print("test three")
  }
}
