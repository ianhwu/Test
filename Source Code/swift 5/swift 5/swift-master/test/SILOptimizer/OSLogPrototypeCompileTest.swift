// RUN: %target-swift-frontend -swift-version 5 -emit-sil -primary-file %s |  %FileCheck %s --check-prefix=CHECK --check-prefix=CHECK-%target-ptrsize
// RUN: %target-swift-frontend -enable-ownership-stripping-after-serialization -swift-version 5 -emit-sil -primary-file %s |  %FileCheck %s --check-prefix=CHECK --check-prefix=CHECK-%target-ptrsize
// REQUIRES: OS=macosx || OS=ios || OS=tvos || OS=watchos

// Tests for the OSLogOptimization pass that performs compile-time analysis
// and optimization of the new os log prototype APIs. The tests here check
// whether specific compile-time constants such as the format string,
// the size of the byte buffer etc. are literals after the mandatory pipeline.

import OSLogPrototype

if #available(OSX 10.12, iOS 10.0, watchOS 3.0, tvOS 10.0, *) {

  // CHECK-LABEL: @$s25OSLogPrototypeCompileTest23testSimpleInterpolationL_1hy0aB06LoggerV_tF
  func testSimpleInterpolation(h: Logger) {
    h.log(level: .debug, "Minimum integer value: \(Int.min)")

    // Check if there is a call to _os_log_impl with a literal format string.
    // CHECK-DAG is used here as it is easier to perform the checks backwards
    // from uses to the definitions.

    // CHECK-DAG: [[OS_LOG_IMPL:%[0-9]+]] = function_ref @_os_log_impl : $@convention(c)
    // CHECK-DAG: apply [[OS_LOG_IMPL]]({{%.*}}, {{%.*}}, {{%.*}}, [[CHARPTR:%[0-9]+]], {{%.*}}, {{%.*}})
    // CHECK-DAG: [[CHARPTR]] = struct $UnsafePointer<Int8> ([[LIT:%[0-9]+]] : $Builtin.RawPointer)
    // CHECK-64-DAG: [[LIT]] = string_literal utf8 "Minimum integer value: %{public}lld"
    // CHECK-32-DAG: [[LIT]] = string_literal utf8 "Minimum integer value: %{public}d"

    // Check if the size of the argument buffer is a constant.

    // CHECK-DAG: [[ALLOCATE:%[0-9]+]] = function_ref @$sSp8allocate8capacitySpyxGSi_tFZ
    // CHECK-DAG: apply [[ALLOCATE]]<UInt8>([[BUFFERSIZE:%[0-9]+]], {{%.*}})
    // CHECK-DAG: [[BUFFERSIZE]] = struct $Int ([[BUFFERSIZELIT:%[0-9]+]]
    // CHECK-64-DAG: [[BUFFERSIZELIT]] = integer_literal $Builtin.Int64, 12
    // CHECK-32-DAG: [[BUFFERSIZELIT]] = integer_literal $Builtin.Int32, 8

    // Check whether the header bytes: premable and argument count are constants.

    // CHECK-DAG: [[SERIALIZE:%[0-9]+]] = function_ref @$s14OSLogPrototype9serialize_2atys5UInt8V_SpyAEGztF
    // CHECK-DAG: apply [[SERIALIZE]]([[PREAMBLE:%[0-9]+]], {{%.*}})
    // CHECK-DAG: [[PREAMBLE]] =  struct $UInt8 ([[PREAMBLELIT:%[0-9]+]] : $Builtin.Int8)
    // CHECK-DAG: [[PREAMBLELIT]] = integer_literal $Builtin.Int8, 0

    // CHECK-DAG: [[SERIALIZE:%[0-9]+]] = function_ref @$s14OSLogPrototype9serialize_2atys5UInt8V_SpyAEGztF
    // CHECK-DAG: apply [[SERIALIZE]]([[ARGCOUNT:%[0-9]+]], {{%.*}})
    // CHECK-DAG: [[ARGCOUNT]] =  struct $UInt8 ([[ARGCOUNTLIT:%[0-9]+]] : $Builtin.Int8)
    // CHECK-DAG: [[ARGCOUNTLIT]] = integer_literal $Builtin.Int8, 1

    // Check whether argument array is folded. We need not check the contents of
    // the array which is checked by a different test suite.

    // CHECK-DAG: [[FOREACH:%[0-9]+]] = function_ref @$sSTsE7forEachyyy7ElementQzKXEKF
    // CHECK-DAG: try_apply [[FOREACH]]<Array<(inout UnsafeMutablePointer<UInt8>, inout Array<AnyObject>) -> ()>>({{%.*}}, [[ARGSARRAYADDR:%[0-9]+]])
    // CHECK-DAG: store [[ARGSARRAY:%[0-9]+]] to [[ARGSARRAYADDR]]
    // CHECK-DAG: [[ARGSARRAY]] = tuple_extract [[ARRAYINITRES:%[0-9]+]] : $(Array<(inout UnsafeMutablePointer<UInt8>, inout Array<AnyObject>) -> ()>, Builtin.RawPointer), 0
    // CHECK-DAG: [[ARRAYINITRES]] = apply [[ARRAYINIT:%[0-9]+]]<(inout UnsafeMutablePointer<UInt8>, inout Array<AnyObject>) -> ()>([[ARRAYSIZE:%[0-9]+]])
    // CHECK-DAG: [[ARRAYINIT]] = function_ref @$ss27_allocateUninitializedArrayySayxG_BptBwlF
    // CHECK-DAG: [[ARRAYSIZE]] = integer_literal $Builtin.Word, 3
  }

  // CHECK-LABEL: @$s25OSLogPrototypeCompileTest34testInterpolationWithFormatOptionsL_1hy0aB06LoggerV_tF
  func testInterpolationWithFormatOptions(h: Logger) {
    h.log(level: .info, "Maximum integer value: \(Int.max, format: .hex)")

    // Check if there is a call to _os_log_impl with a literal format string.

    // CHECK-DAG: [[OS_LOG_IMPL:%[0-9]+]] = function_ref @_os_log_impl : $@convention(c)
    // CHECK-DAG: apply [[OS_LOG_IMPL]]({{%.*}}, {{%.*}}, {{%.*}}, [[CHARPTR:%[0-9]+]], {{%.*}}, {{%.*}})
    // CHECK-DAG: [[CHARPTR]] = struct $UnsafePointer<Int8> ([[LIT:%[0-9]+]] : $Builtin.RawPointer)
    // CHECK-64-DAG: [[LIT]] = string_literal utf8 "Maximum integer value: %{public}llx"
    // CHECK-32-DAG: [[LIT]] = string_literal utf8 "Maximum integer value: %{public}x"

    // Check if the size of the argument buffer is a constant.

    // CHECK-DAG: [[ALLOCATE:%[0-9]+]] = function_ref @$sSp8allocate8capacitySpyxGSi_tFZ
    // CHECK-DAG: apply [[ALLOCATE]]<UInt8>([[BUFFERSIZE:%[0-9]+]], {{%.*}})
    // CHECK-DAG: [[BUFFERSIZE]] = struct $Int ([[BUFFERSIZELIT:%[0-9]+]]
    // CHECK-64-DAG: [[BUFFERSIZELIT]] = integer_literal $Builtin.Int64, 12
    // CHECK-32-DAG: [[BUFFERSIZELIT]] = integer_literal $Builtin.Int32, 8

    // Check whether the header bytes: premable and argument count are constants.

    // CHECK-DAG: [[SERIALIZE:%[0-9]+]] = function_ref @$s14OSLogPrototype9serialize_2atys5UInt8V_SpyAEGztF
    // CHECK-DAG: apply [[SERIALIZE]]([[PREAMBLE:%[0-9]+]], {{%.*}})
    // CHECK-DAG: [[PREAMBLE]] =  struct $UInt8 ([[PREAMBLELIT:%[0-9]+]] : $Builtin.Int8)
    // CHECK-DAG: [[PREAMBLELIT]] = integer_literal $Builtin.Int8, 0

    // CHECK-DAG: [[SERIALIZE:%[0-9]+]] = function_ref @$s14OSLogPrototype9serialize_2atys5UInt8V_SpyAEGztF
    // CHECK-DAG: apply [[SERIALIZE]]([[ARGCOUNT:%[0-9]+]], {{%.*}})
    // CHECK-DAG: [[ARGCOUNT]] =  struct $UInt8 ([[ARGCOUNTLIT:%[0-9]+]] : $Builtin.Int8)
    // CHECK-DAG: [[ARGCOUNTLIT]] = integer_literal $Builtin.Int8, 1

    // Check whether argument array is folded. We need not check the contents of
    // the array which is checked by a different test suite.

    // CHECK-DAG: [[FOREACH:%[0-9]+]] = function_ref @$sSTsE7forEachyyy7ElementQzKXEKF
    // CHECK-DAG: try_apply [[FOREACH]]<Array<(inout UnsafeMutablePointer<UInt8>, inout Array<AnyObject>) -> ()>>({{%.*}}, [[ARGSARRAYADDR:%[0-9]+]])
    // CHECK-DAG: store [[ARGSARRAY:%[0-9]+]] to [[ARGSARRAYADDR]]
    // CHECK-DAG: [[ARGSARRAY]] = tuple_extract [[ARRAYINITRES:%[0-9]+]] : $(Array<(inout UnsafeMutablePointer<UInt8>, inout Array<AnyObject>) -> ()>, Builtin.RawPointer), 0
    // CHECK-DAG: [[ARRAYINITRES]] = apply [[ARRAYINIT:%[0-9]+]]<(inout UnsafeMutablePointer<UInt8>, inout Array<AnyObject>) -> ()>([[ARRAYSIZE:%[0-9]+]])
    // CHECK-DAG: [[ARRAYINIT]] = function_ref @$ss27_allocateUninitializedArrayySayxG_BptBwlF
    // CHECK-DAG: [[ARRAYSIZE]] = integer_literal $Builtin.Word, 3
  }

  // CHECK-LABEL: @$s25OSLogPrototypeCompileTest44testInterpolationWithFormatOptionsAndPrivacyL_1hy0aB06LoggerV_tF
  func testInterpolationWithFormatOptionsAndPrivacy(h: Logger) {
    let privateID = 0x79abcdef
    h.log(
      level: .error,
      "Private Identifier: \(privateID, format: .hex, privacy: .private)")

    // Check if there is a call to _os_log_impl with a literal format string.

    // CHECK-DAG: [[OS_LOG_IMPL:%[0-9]+]] = function_ref @_os_log_impl : $@convention(c)
    // CHECK-DAG: apply [[OS_LOG_IMPL]]({{%.*}}, {{%.*}}, {{%.*}}, [[CHARPTR:%[0-9]+]], {{%.*}}, {{%.*}})
    // CHECK-DAG: [[CHARPTR]] = struct $UnsafePointer<Int8> ([[LIT:%[0-9]+]] : $Builtin.RawPointer)
    // CHECK-64-DAG: [[LIT]] = string_literal utf8 "Private Identifier: %{private}llx"
    // CHECK-32-DAG: [[LIT]] = string_literal utf8 "Private Identifier: %{private}x"

    // Check if the size of the argument buffer is a constant.

    // CHECK-DAG: [[ALLOCATE:%[0-9]+]] = function_ref @$sSp8allocate8capacitySpyxGSi_tFZ
    // CHECK-DAG: apply [[ALLOCATE]]<UInt8>([[BUFFERSIZE:%[0-9]+]], {{%.*}})
    // CHECK-DAG: [[BUFFERSIZE]] = struct $Int ([[BUFFERSIZELIT:%[0-9]+]]
    // CHECK-64-DAG: [[BUFFERSIZELIT]] = integer_literal $Builtin.Int64, 12
    // CHECK-32-DAG: [[BUFFERSIZELIT]] = integer_literal $Builtin.Int32, 8

    // Check whether the header bytes: premable and argument count are constants.

    // CHECK-DAG: [[SERIALIZE:%[0-9]+]] = function_ref @$s14OSLogPrototype9serialize_2atys5UInt8V_SpyAEGztF
    // CHECK-DAG: apply [[SERIALIZE]]([[PREAMBLE:%[0-9]+]], {{%.*}})
    // CHECK-DAG: [[PREAMBLE]] =  struct $UInt8 ([[PREAMBLELIT:%[0-9]+]] : $Builtin.Int8)
    // CHECK-DAG: [[PREAMBLELIT]] = integer_literal $Builtin.Int8, 1

    // CHECK-DAG: [[SERIALIZE:%[0-9]+]] = function_ref @$s14OSLogPrototype9serialize_2atys5UInt8V_SpyAEGztF
    // CHECK-DAG: apply [[SERIALIZE]]([[ARGCOUNT:%[0-9]+]], {{%.*}})
    // CHECK-DAG: [[ARGCOUNT]] =  struct $UInt8 ([[ARGCOUNTLIT:%[0-9]+]] : $Builtin.Int8)
    // CHECK-DAG: [[ARGCOUNTLIT]] = integer_literal $Builtin.Int8, 1

    // Check whether argument array is folded. We need not check the contents of
    // the array which is checked by a different test suite.

    // CHECK-DAG: [[FOREACH:%[0-9]+]] = function_ref @$sSTsE7forEachyyy7ElementQzKXEKF
    // CHECK-DAG: try_apply [[FOREACH]]<Array<(inout UnsafeMutablePointer<UInt8>, inout Array<AnyObject>) -> ()>>({{%.*}}, [[ARGSARRAYADDR:%[0-9]+]])
    // CHECK-DAG: store [[ARGSARRAY:%[0-9]+]] to [[ARGSARRAYADDR]]
    // CHECK-DAG: [[ARGSARRAY]] = tuple_extract [[ARRAYINITRES:%[0-9]+]] : $(Array<(inout UnsafeMutablePointer<UInt8>, inout Array<AnyObject>) -> ()>, Builtin.RawPointer), 0
    // CHECK-DAG: [[ARRAYINITRES]] = apply [[ARRAYINIT:%[0-9]+]]<(inout UnsafeMutablePointer<UInt8>, inout Array<AnyObject>) -> ()>([[ARRAYSIZE:%[0-9]+]])
    // CHECK-DAG: [[ARRAYINIT]] = function_ref @$ss27_allocateUninitializedArrayySayxG_BptBwlF
    // CHECK-DAG: [[ARRAYSIZE]] = integer_literal $Builtin.Word, 3
  }

  // CHECK-LABEL: @$s25OSLogPrototypeCompileTest38testInterpolationWithMultipleArgumentsL_1hy0aB06LoggerV_tF
  func testInterpolationWithMultipleArguments(h: Logger) {
    let privateID = 0x79abcdef
    let filePermissions = 0o777
    let pid = 122225
    h.log(
      level: .error,
      """
      Access prevented: process \(pid) initiated by \
      user: \(privateID, privacy: .private) attempted resetting \
      permissions to \(filePermissions, format: .octal)
      """)

    // Check if there is a call to _os_log_impl with a literal format string.

    // CHECK-DAG: [[OS_LOG_IMPL:%[0-9]+]] = function_ref @_os_log_impl : $@convention(c)
    // CHECK-DAG: apply [[OS_LOG_IMPL]]({{%.*}}, {{%.*}}, {{%.*}}, [[CHARPTR:%[0-9]+]], {{%.*}}, {{%.*}})
    // CHECK-DAG: [[CHARPTR]] = struct $UnsafePointer<Int8> ([[LIT:%[0-9]+]] : $Builtin.RawPointer)
    // CHECK-64-DAG: [[LIT]] = string_literal utf8 "Access prevented: process %{public}lld initiated by user: %{private}lld attempted resetting permissions to %{public}llo"
    // CHECK-32-DAG: [[LIT]] = string_literal utf8 "Access prevented: process %{public}d initiated by user: %{private}d attempted resetting permissions to %{public}o"

    // Check if the size of the argument buffer is a constant.

    // CHECK-DAG: [[ALLOCATE:%[0-9]+]] = function_ref @$sSp8allocate8capacitySpyxGSi_tFZ
    // CHECK-DAG: apply [[ALLOCATE]]<UInt8>([[BUFFERSIZE:%[0-9]+]], {{%.*}})
    // CHECK-DAG: [[BUFFERSIZE]] = struct $Int ([[BUFFERSIZELIT:%[0-9]+]]
    // CHECK-64-DAG: [[BUFFERSIZELIT]] = integer_literal $Builtin.Int64, 32
    // CHECK-32-DAG: [[BUFFERSIZELIT]] = integer_literal $Builtin.Int32, 20

    // Check whether the header bytes: premable and argument count are constants.

    // CHECK-DAG: [[SERIALIZE:%[0-9]+]] = function_ref @$s14OSLogPrototype9serialize_2atys5UInt8V_SpyAEGztF
    // CHECK-DAG: apply [[SERIALIZE]]([[PREAMBLE:%[0-9]+]], {{%.*}})
    // CHECK-DAG: [[PREAMBLE]] =  struct $UInt8 ([[PREAMBLELIT:%[0-9]+]] : $Builtin.Int8)
    // CHECK-DAG: [[PREAMBLELIT]] = integer_literal $Builtin.Int8, 1

    // CHECK-DAG: [[SERIALIZE:%[0-9]+]] = function_ref @$s14OSLogPrototype9serialize_2atys5UInt8V_SpyAEGztF
    // CHECK-DAG: apply [[SERIALIZE]]([[ARGCOUNT:%[0-9]+]], {{%.*}})
    // CHECK-DAG: [[ARGCOUNT]] =  struct $UInt8 ([[ARGCOUNTLIT:%[0-9]+]] : $Builtin.Int8)
    // CHECK-DAG: [[ARGCOUNTLIT]] = integer_literal $Builtin.Int8, 3

    // Check whether argument array is folded. We need not check the contents of
    // the array which is checked by a different test suite.

    // CHECK-DAG: [[FOREACH:%[0-9]+]] = function_ref @$sSTsE7forEachyyy7ElementQzKXEKF
    // CHECK-DAG: try_apply [[FOREACH]]<Array<(inout UnsafeMutablePointer<UInt8>, inout Array<AnyObject>) -> ()>>({{%.*}}, [[ARGSARRAYADDR:%[0-9]+]])
    // CHECK-DAG: store [[ARGSARRAY:%[0-9]+]] to [[ARGSARRAYADDR]]
    // CHECK-DAG: [[ARGSARRAY]] = tuple_extract [[ARRAYINITRES:%[0-9]+]] : $(Array<(inout UnsafeMutablePointer<UInt8>, inout Array<AnyObject>) -> ()>, Builtin.RawPointer), 0
    // CHECK-DAG: [[ARRAYINITRES]] = apply [[ARRAYINIT:%[0-9]+]]<(inout UnsafeMutablePointer<UInt8>, inout Array<AnyObject>) -> ()>([[ARRAYSIZE:%[0-9]+]])
    // CHECK-DAG: [[ARRAYINIT]] = function_ref @$ss27_allocateUninitializedArrayySayxG_BptBwlF
    // CHECK-DAG: [[ARRAYSIZE]] = integer_literal $Builtin.Word, 9
  }

  // CHECK-LABEL: @$s25OSLogPrototypeCompileTest25testLogMessageWithoutDataL_1hy0aB06LoggerV_tF
  func testLogMessageWithoutData(h: Logger) {
    // FIXME: here `ExpressibleByStringLiteral` conformance of OSLogMessage
    // is used. In this case, the constant evaluation begins from the apply of
    // the "string.makeUTF8: initializer. The constant evaluator ends up using
    // the backward mode to identify the string_literal inst passed to the
    // initializer. Eliminate  reliance on this backward mode by starting from
    // the string_literal inst, instead of initialization instruction.
    h.log("A message with no data")

    // Check if there is a call to _os_log_impl with a literal format string.

    // CHECK-DAG: [[OS_LOG_IMPL:%[0-9]+]] = function_ref @_os_log_impl : $@convention(c)
    // CHECK-DAG: apply [[OS_LOG_IMPL]]({{%.*}}, {{%.*}}, {{%.*}}, [[CHARPTR:%[0-9]+]], {{%.*}}, {{%.*}})
    // CHECK-DAG: [[CHARPTR]] = struct $UnsafePointer<Int8> ([[LIT:%[0-9]+]] : $Builtin.RawPointer)
    // CHECK-DAG: [[LIT]] = string_literal utf8 "A message with no data"

    // Check if the size of the argument buffer is a constant.

    // CHECK-DAG: [[ALLOCATE:%[0-9]+]] = function_ref @$sSp8allocate8capacitySpyxGSi_tFZ
    // CHECK-DAG: apply [[ALLOCATE]]<UInt8>([[BUFFERSIZE:%[0-9]+]], {{%.*}})
    // CHECK-DAG: [[BUFFERSIZE]] = struct $Int ([[BUFFERSIZELIT:%[0-9]+]]
    // CHECK-DAG: [[BUFFERSIZELIT]] = integer_literal $Builtin.Int{{[0-9]+}}, 2

    // Check whether the header bytes: premable and argument count are constants.

    // CHECK-DAG: [[SERIALIZE:%[0-9]+]] = function_ref @$s14OSLogPrototype9serialize_2atys5UInt8V_SpyAEGztF
    // CHECK-DAG: apply [[SERIALIZE]]([[PREAMBLE:%[0-9]+]], {{%.*}})
    // CHECK-DAG: [[PREAMBLE]] =  struct $UInt8 ([[PREAMBLELIT:%[0-9]+]] : $Builtin.Int8)
    // CHECK-DAG: [[PREAMBLELIT]] = integer_literal $Builtin.Int8, 0

    // CHECK-DAG: [[SERIALIZE:%[0-9]+]] = function_ref @$s14OSLogPrototype9serialize_2atys5UInt8V_SpyAEGztF
    // CHECK-DAG: apply [[SERIALIZE]]([[ARGCOUNT:%[0-9]+]], {{%.*}})
    // CHECK-DAG: [[ARGCOUNT]] =  struct $UInt8 ([[ARGCOUNTLIT:%[0-9]+]] : $Builtin.Int8)
    // CHECK-DAG: [[ARGCOUNTLIT]] = integer_literal $Builtin.Int8, 0

    // Check whether argument array is folded.

    // CHECK-DAG: [[FOREACH:%[0-9]+]] = function_ref @$sSTsE7forEachyyy7ElementQzKXEKF
    // CHECK-DAG: try_apply [[FOREACH]]<Array<(inout UnsafeMutablePointer<UInt8>, inout Array<AnyObject>) -> ()>>({{%.*}}, [[ARGSARRAYADDR:%[0-9]+]])
    // CHECK-DAG: store [[ARGSARRAY:%[0-9]+]] to [[ARGSARRAYADDR]]
    // CHECK-DAG: [[ARGSARRAY]] = tuple_extract [[ARRAYINITRES:%[0-9]+]] : $(Array<(inout UnsafeMutablePointer<UInt8>, inout Array<AnyObject>) -> ()>, Builtin.RawPointer), 0
    // CHECK-DAG: [[ARRAYINITRES]] = apply [[ARRAYINIT:%[0-9]+]]<(inout UnsafeMutablePointer<UInt8>, inout Array<AnyObject>) -> ()>([[ARRAYSIZE:%[0-9]+]])
    // CHECK-DAG: [[ARRAYINIT]] = function_ref @$ss27_allocateUninitializedArrayySayxG_BptBwlF
    // CHECK-DAG: [[ARRAYSIZE]] = integer_literal $Builtin.Word, 0
  }

  // CHECK-LABEL: @$s25OSLogPrototypeCompileTest22testEscapingOfPercentsL_1hy0aB06LoggerV_tF
  func testEscapingOfPercents(h: Logger) {
    h.log("Process failed after 99% completion")
    // CHECK-DAG: [[OS_LOG_IMPL:%[0-9]+]] = function_ref @_os_log_impl : $@convention(c)
    // CHECK-DAG: apply [[OS_LOG_IMPL]]({{%.*}}, {{%.*}}, {{%.*}}, [[CHARPTR:%[0-9]+]], {{%.*}}, {{%.*}})
    // CHECK-DAG: [[CHARPTR]] = struct $UnsafePointer<Int8> ([[LIT:%[0-9]+]] : $Builtin.RawPointer)
    // CHECK-DAG: [[LIT]] = string_literal utf8 "Process failed after 99%% completion"
  }

  // CHECK-LABEL: @$s25OSLogPrototypeCompileTest18testDoublePercentsL_1hy0aB06LoggerV_tF
  func testDoublePercents(h: Logger) {
    h.log("Double percents: %%")
    // CHECK-DAG: [[OS_LOG_IMPL:%[0-9]+]] = function_ref @_os_log_impl : $@convention(c)
    // CHECK-DAG: apply [[OS_LOG_IMPL]]({{%.*}}, {{%.*}}, {{%.*}}, [[CHARPTR:%[0-9]+]], {{%.*}}, {{%.*}})
    // CHECK-DAG: [[CHARPTR]] = struct $UnsafePointer<Int8> ([[LIT:%[0-9]+]] : $Builtin.RawPointer)
    // CHECK-DAG: [[LIT]] = string_literal utf8 "Double percents: %%%%"
  }

  // CHECK-LABEL: @$s25OSLogPrototypeCompileTest22testSmallFormatStringsL_1hy0aB06LoggerV_tF
  func testSmallFormatStrings(h: Logger) {
    h.log("a")
    // CHECK-DAG: [[OS_LOG_IMPL:%[0-9]+]] = function_ref @_os_log_impl : $@convention(c)
    // CHECK-DAG: apply [[OS_LOG_IMPL]]({{%.*}}, {{%.*}}, {{%.*}}, [[CHARPTR:%[0-9]+]], {{%.*}}, {{%.*}})
    // CHECK-DAG: [[CHARPTR]] = struct $UnsafePointer<Int8> ([[LIT:%[0-9]+]] : $Builtin.RawPointer)
    // CHECK-DAG: [[LIT]] = string_literal utf8 "a"
  }

  /// A stress test that checks whether the optimizer handle messages with more
  /// than 48 interpolated expressions. Interpolated expressions beyond this
  /// limit must be ignored.
  // CHECK-LABEL: @$s25OSLogPrototypeCompileTest31testMessageWithTooManyArgumentsL_1hy0aB06LoggerV_tF
  func testMessageWithTooManyArguments(h: Logger) {
    h.log(
      level: .error,
      """
      \(1) \(1) \(1) \(1) \(1) \(1) \(1) \(1) \(1) \(1) \(1) \(1) \(1) \(1) \
      \(1) \(1) \(1) \(1) \(1) \(1) \(1) \(1) \(1) \(1) \(1) \(1) \(1) \(1) \
      \(1) \(1) \(1) \(1) \(1) \(1) \(1) \(1) \(1) \(1) \(1) \(1) \(1) \(1) \
      \(1) \(1) \(1) \(1) \(1) \(48) \(49)
      """)

    // Check if there is a call to _os_log_impl with a literal format string.

    // CHECK-DAG: [[OS_LOG_IMPL:%[0-9]+]] = function_ref @_os_log_impl : $@convention(c)
    // CHECK-DAG: apply [[OS_LOG_IMPL]]({{%.*}}, {{%.*}}, {{%.*}}, [[CHARPTR:%[0-9]+]], {{%.*}}, {{%.*}})
    // CHECK-DAG: [[CHARPTR]] = struct $UnsafePointer<Int8> ([[LIT:%[0-9]+]] : $Builtin.RawPointer)
    // CHECK-64-DAG: [[LIT]] = string_literal utf8 "%{public}lld %{public}lld %{public}lld %{public}lld %{public}lld %{public}lld %{public}lld %{public}lld %{public}lld %{public}lld %{public}lld %{public}lld %{public}lld %{public}lld %{public}lld %{public}lld %{public}lld %{public}lld %{public}lld %{public}lld %{public}lld %{public}lld %{public}lld %{public}lld %{public}lld %{public}lld %{public}lld %{public}lld %{public}lld %{public}lld %{public}lld %{public}lld %{public}lld %{public}lld %{public}lld %{public}lld %{public}lld %{public}lld %{public}lld %{public}lld %{public}lld %{public}lld %{public}lld %{public}lld %{public}lld %{public}lld %{public}lld %{public}lld "
    // CHECK-32-DAG: [[LIT]] = string_literal utf8 "%{public}d %{public}d %{public}d %{public}d %{public}d %{public}d %{public}d %{public}d %{public}d %{public}d %{public}d %{public}d %{public}d %{public}d %{public}d %{public}d %{public}d %{public}d %{public}d %{public}d %{public}d %{public}d %{public}d %{public}d %{public}d %{public}d %{public}d %{public}d %{public}d %{public}d %{public}d %{public}d %{public}d %{public}d %{public}d %{public}d %{public}d %{public}d %{public}d %{public}d %{public}d %{public}d %{public}d %{public}d %{public}d %{public}d %{public}d %{public}d "

    // Check if the size of the argument buffer is a constant.

    // CHECK-DAG: [[ALLOCATE:%[0-9]+]] = function_ref @$sSp8allocate8capacitySpyxGSi_tFZ
    // CHECK-DAG: apply [[ALLOCATE]]<UInt8>([[BUFFERSIZE:%[0-9]+]], {{%.*}})
    // CHECK-DAG: [[BUFFERSIZE]] = struct $Int ([[BUFFERSIZELIT:%[0-9]+]]
    // CHECK-64-DAG: [[BUFFERSIZELIT]] = integer_literal $Builtin.Int64, 482
    // CHECK-32-DAG: [[BUFFERSIZELIT]] = integer_literal $Builtin.Int32, 290

    // Check whether the header bytes: premable and argument count are constants.

    // CHECK-DAG: [[SERIALIZE:%[0-9]+]] = function_ref @$s14OSLogPrototype9serialize_2atys5UInt8V_SpyAEGztF
    // CHECK-DAG: apply [[SERIALIZE]]([[PREAMBLE:%[0-9]+]], {{%.*}})
    // CHECK-DAG: [[PREAMBLE]] =  struct $UInt8 ([[PREAMBLELIT:%[0-9]+]] : $Builtin.Int8)
    // CHECK-DAG: [[PREAMBLELIT]] = integer_literal $Builtin.Int8, 0

    // CHECK-DAG: [[SERIALIZE:%[0-9]+]] = function_ref @$s14OSLogPrototype9serialize_2atys5UInt8V_SpyAEGztF
    // CHECK-DAG: apply [[SERIALIZE]]([[ARGCOUNT:%[0-9]+]], {{%.*}})
    // CHECK-DAG: [[ARGCOUNT]] =  struct $UInt8 ([[ARGCOUNTLIT:%[0-9]+]] : $Builtin.Int8)
    // CHECK-DAG: [[ARGCOUNTLIT]] = integer_literal $Builtin.Int8, 48

    // Check whether argument array is folded.

    // CHECK-DAG: [[FOREACH:%[0-9]+]] = function_ref @$sSTsE7forEachyyy7ElementQzKXEKF
    // CHECK-DAG: try_apply [[FOREACH]]<Array<(inout UnsafeMutablePointer<UInt8>, inout Array<AnyObject>) -> ()>>({{%.*}}, [[ARGSARRAYADDR:%[0-9]+]])
    // CHECK-DAG: store [[ARGSARRAY:%[0-9]+]] to [[ARGSARRAYADDR]]
    // CHECK-DAG: [[ARGSARRAY]] = tuple_extract [[ARRAYINITRES:%[0-9]+]] : $(Array<(inout UnsafeMutablePointer<UInt8>, inout Array<AnyObject>) -> ()>, Builtin.RawPointer), 0
    // CHECK-DAG: [[ARRAYINITRES]] = apply [[ARRAYINIT:%[0-9]+]]<(inout UnsafeMutablePointer<UInt8>, inout Array<AnyObject>) -> ()>([[ARRAYSIZE:%[0-9]+]])
    // CHECK-DAG: [[ARRAYINIT]] = function_ref @$ss27_allocateUninitializedArrayySayxG_BptBwlF
    // CHECK-DAG: [[ARRAYSIZE]] = integer_literal $Builtin.Word, 144
  }

  // CHECK-LABEL: @$s25OSLogPrototypeCompileTest22testInt32InterpolationL_1hy0aB06LoggerV_tF
  func testInt32Interpolation(h: Logger) {
    h.log("32-bit integer value: \(Int32.min)")

    // Check if there is a call to _os_log_impl with a literal format string.

    // CHECK-DAG: [[OS_LOG_IMPL:%[0-9]+]] = function_ref @_os_log_impl : $@convention(c)
    // CHECK-DAG: apply [[OS_LOG_IMPL]]({{%.*}}, {{%.*}}, {{%.*}}, [[CHARPTR:%[0-9]+]], {{%.*}}, {{%.*}})
    // CHECK-DAG: [[CHARPTR]] = struct $UnsafePointer<Int8> ([[LIT:%[0-9]+]] : $Builtin.RawPointer)
    // CHECK-DAG: [[LIT]] = string_literal utf8 "32-bit integer value: %{public}d"

    // Check if the size of the argument buffer is a constant.

    // CHECK-DAG: [[ALLOCATE:%[0-9]+]] = function_ref @$sSp8allocate8capacitySpyxGSi_tFZ
    // CHECK-DAG: apply [[ALLOCATE]]<UInt8>([[BUFFERSIZE:%[0-9]+]], {{%.*}})
    // CHECK-DAG: [[BUFFERSIZE]] = struct $Int ([[BUFFERSIZELIT:%[0-9]+]]
    // CHECK-64-DAG: [[BUFFERSIZELIT]] = integer_literal $Builtin.Int64, 8
    // CHECK-32-DAG: [[BUFFERSIZELIT]] = integer_literal $Builtin.Int32, 8

    // Check whether the header bytes: premable and argument count are constants.

    // CHECK-DAG: [[SERIALIZE:%[0-9]+]] = function_ref @$s14OSLogPrototype9serialize_2atys5UInt8V_SpyAEGztF
    // CHECK-DAG: apply [[SERIALIZE]]([[PREAMBLE:%[0-9]+]], {{%.*}})
    // CHECK-DAG: [[PREAMBLE]] =  struct $UInt8 ([[PREAMBLELIT:%[0-9]+]] : $Builtin.Int8)
    // CHECK-DAG: [[PREAMBLELIT]] = integer_literal $Builtin.Int8, 0

    // CHECK-DAG: [[SERIALIZE:%[0-9]+]] = function_ref @$s14OSLogPrototype9serialize_2atys5UInt8V_SpyAEGztF
    // CHECK-DAG: apply [[SERIALIZE]]([[ARGCOUNT:%[0-9]+]], {{%.*}})
    // CHECK-DAG: [[ARGCOUNT]] =  struct $UInt8 ([[ARGCOUNTLIT:%[0-9]+]] : $Builtin.Int8)
    // CHECK-DAG: [[ARGCOUNTLIT]] = integer_literal $Builtin.Int8, 1
  }

  // CHECK-LABEL: @$s25OSLogPrototypeCompileTest26testDynamicStringArgumentsL_1hy0aB06LoggerV_tF
  func testDynamicStringArguments(h: Logger) {
    let concatString = "hello" + " - " + "world"
    let interpolatedString = "\(31) trillion digits of pi are known so far"

    h.log("""
      concat: \(concatString, privacy: .public) \
      interpolated: \(interpolatedString, privacy: .private)
      """)

    // Check if there is a call to _os_log_impl with a literal format string.
    // CHECK-DAG is used here as it is easier to perform the checks backwards
    // from uses to the definitions.

    // CHECK-DAG: [[OS_LOG_IMPL:%[0-9]+]] = function_ref @_os_log_impl : $@convention(c)
    // CHECK-DAG: apply [[OS_LOG_IMPL]]({{%.*}}, {{%.*}}, {{%.*}}, [[CHARPTR:%[0-9]+]], {{%.*}}, {{%.*}})
    // CHECK-DAG: [[CHARPTR]] = struct $UnsafePointer<Int8> ([[LIT:%[0-9]+]] : $Builtin.RawPointer)
    // CHECK-DAG: [[LIT]] = string_literal utf8 "concat: %{public}s interpolated: %{private}s"

    // Check if the size of the argument buffer is a constant.

    // CHECK-DAG: [[ALLOCATE:%[0-9]+]] = function_ref @$sSp8allocate8capacitySpyxGSi_tFZ
    // CHECK-DAG: apply [[ALLOCATE]]<UInt8>([[BUFFERSIZE:%[0-9]+]], {{%.*}})
    // CHECK-DAG: [[BUFFERSIZE]] = struct $Int ([[BUFFERSIZELIT:%[0-9]+]]
    // CHECK-64-DAG: [[BUFFERSIZELIT]] = integer_literal $Builtin.Int64, 22
    // CHECK-32-DAG: [[BUFFERSIZELIT]] = integer_literal $Builtin.Int32, 14

    // Check whether the header bytes: premable and argument count are constants.

    // CHECK-DAG: [[SERIALIZE:%[0-9]+]] = function_ref @$s14OSLogPrototype9serialize_2atys5UInt8V_SpyAEGztF
    // CHECK-DAG: apply [[SERIALIZE]]([[PREAMBLE:%[0-9]+]], {{%.*}})
    // CHECK-DAG: [[PREAMBLE]] =  struct $UInt8 ([[PREAMBLELIT:%[0-9]+]] : $Builtin.Int8)
    // CHECK-DAG: [[PREAMBLELIT]] = integer_literal $Builtin.Int8, 3

    // CHECK-DAG: [[SERIALIZE:%[0-9]+]] = function_ref @$s14OSLogPrototype9serialize_2atys5UInt8V_SpyAEGztF
    // CHECK-DAG: apply [[SERIALIZE]]([[ARGCOUNT:%[0-9]+]], {{%.*}})
    // CHECK-DAG: [[ARGCOUNT]] =  struct $UInt8 ([[ARGCOUNTLIT:%[0-9]+]] : $Builtin.Int8)
    // CHECK-DAG: [[ARGCOUNTLIT]] = integer_literal $Builtin.Int8, 2

    // Check whether argument array is folded. We need not check the contents of
    // the array which is checked by a different test suite.

    // CHECK-DAG: [[FOREACH:%[0-9]+]] = function_ref @$sSTsE7forEachyyy7ElementQzKXEKF
    // CHECK-DAG: try_apply [[FOREACH]]<Array<(inout UnsafeMutablePointer<UInt8>, inout Array<AnyObject>) -> ()>>({{%.*}}, [[ARGSARRAYADDR:%[0-9]+]])
    // CHECK-DAG: store [[ARGSARRAY:%[0-9]+]] to [[ARGSARRAYADDR]]
    // CHECK-DAG: [[ARGSARRAY]] = tuple_extract [[ARRAYINITRES:%[0-9]+]] : $(Array<(inout UnsafeMutablePointer<UInt8>, inout Array<AnyObject>) -> ()>, Builtin.RawPointer), 0
    // CHECK-DAG: [[ARRAYINITRES]] = apply [[ARRAYINIT:%[0-9]+]]<(inout UnsafeMutablePointer<UInt8>, inout Array<AnyObject>) -> ()>([[ARRAYSIZE:%[0-9]+]])
    // CHECK-DAG: [[ARRAYINIT]] = function_ref @$ss27_allocateUninitializedArrayySayxG_BptBwlF
    // CHECK-DAG: [[ARRAYSIZE]] = integer_literal $Builtin.Word, 6
  }
}

