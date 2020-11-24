//===----------------- OSLogMessage.swift ---------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2019 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

// This file contains data structures and helper functions that are used by
// the new OS log APIs. These are prototype implementations and should not be
// used outside of tests.

/// Formatting options supported by the logging APIs for logging integers.
/// These can be specified in the string interpolation passed to the log APIs.
/// For Example,
///     log.info("Writing to file with permissions: \(perm, format: .octal)")
///
/// See `OSLogInterpolation.appendInterpolation` definitions for default options
/// for integer types.
public enum IntFormat {
  case decimal
  case hex
  case octal
}

/// Privacy qualifiers for indicating the privacy level of the logged data
/// to the logging system. These can be specified in the string interpolation
/// passed to the log APIs.
/// For Example,
///     log.info("Login request from user id \(userid, privacy: .private)")
///
/// See `OSLogInterpolation.appendInterpolation` definitions for default options
/// for each supported type.
public enum Privacy {
  case `private`
  case `public`
}

/// Maximum number of arguments i.e., interpolated expressions that can
/// be used in the string interpolations passed to the log APIs.
/// This limit is imposed by the ABI of os_log.
@_transparent
public var maxOSLogArgumentCount: UInt8 { return 48 }

@usableFromInline
@_transparent
internal var logBitsPerByte: Int { return 3 }

/// Represents a string interpolation passed to the log APIs.
///
/// This type converts (through its methods) the given string interpolation into
/// a C-style format string and a sequence of arguments, which is represented
/// by the type `OSLogArguments`.
///
/// Do not create an instance of this type directly. It is used by the compiler
/// when you pass a string interpolation to the log APIs.
/// Extend this type with more `appendInterpolation` overloads to enable
/// interpolating additional types.
@frozen
public struct OSLogInterpolation : StringInterpolationProtocol {
  /// A format string constructed from the given string interpolation to be
  /// passed to the os_log ABI.
  @usableFromInline
  internal var formatString: String

  /// A representation of a sequence of arguments that must be serialized
  /// to a byte buffer and passed to the os_log ABI. Each argument, which is
  /// an (autoclosured) expressions that is interpolated, is prepended with a
  /// two byte header. The first header byte consists of a four bit flag and
  /// a four bit type. The second header byte has the size of the argument in
  /// bytes. This is schematically illustrated below.
  ///                 ----------------------------
  ///                 | 4-bit type  | 4-bit flag  |
  ///                 ----------------------------
  ///                 | 1st argument size in bytes|
  ///                 ----------------------------
  ///                 |     1st argument bytes    |
  ///                 ----------------------------
  ///                 | 4-bit type  | 4-bit flag  |
  ///                 -----------------------------
  ///                 | 2nd argument size in bytes|
  ///                 ----------------------------
  ///                 |     2nd argument bytes    |
  ///                 ----------------------------
  ///                         ...
  @usableFromInline
  internal var arguments: OSLogArguments

  /// The possible values for the argument flag, as defined by the os_log ABI,
  /// which occupies four least significant bits of the first byte of the
  /// argument header. The first two bits are used to indicate privacy and
  /// the other two are reserved.
  @usableFromInline
  @frozen
  internal enum ArgumentFlag {
    case privateFlag
    case publicFlag

    @inlinable
    internal var rawValue: UInt8 {
      switch self {
      case .privateFlag:
        return 0x1
      case .publicFlag:
        return 0x2
      }
    }
  }

  /// The possible values for the argument type, as defined by the os_log ABI,
  /// which occupies four most significant bits of the first byte of the
  /// argument header. The rawValue of this enum must be constant evaluable.
  /// (Note that an auto-generated rawValue is not constant evaluable because
  /// it cannot be annotated so.)
  @usableFromInline
  @frozen
  internal enum ArgumentType {
    case scalar, count, string, pointer, object

    @inlinable
    internal var rawValue: UInt8 {
      switch self {
      case .scalar:
        return 0
      case .count:
        return 1
      case .string:
        return 2
      case .pointer:
        return 3
      case .object:
        return 4
      }
    }
  }

  /// The first summary byte in the byte buffer passed to the os_log ABI that
  /// summarizes the privacy and nature of the arguments.
  @usableFromInline
  internal var preamble: UInt8

  /// Bit mask for setting bits in the peamble. The bits denoted by the bit
  /// mask indicate whether there is an argument that is private, and whether
  /// there is an argument that is non-scalar: String, NSObject or Pointer.
  @usableFromInline
  @frozen
  internal enum PreambleBitMask {
    case privateBitMask
    case nonScalarBitMask

    @inlinable
    internal var rawValue: UInt8 {
      switch self {
      case .privateBitMask:
        return 0x1
      case .nonScalarBitMask:
        return 0x2
      }
    }
  }

  /// The second summary byte that denotes the number of arguments, which is
  /// also the number of interpolated expressions. This will be determined
  /// on the fly in order to support concatenation and interpolation of
  /// instances of `OSLogMessage`.
  @usableFromInline
  internal var argumentCount: UInt8

  /// Sum total of all the bytes (including header bytes) needed for
  /// serializing the arguments.
  @usableFromInline
  internal var totalBytesForSerializingArguments: Int

  // Some methods defined below are marked @_optimize(none) to prevent inlining
  // of string internals (such as String._StringGuts) which will interfere with
  // constant evaluation and folding. Note that these methods will be inlined,
  // constant evaluated/folded and optimized in the context of a caller.

  @_semantics("oslog.interpolation.init")
  @_semantics("constant_evaluable")
  @inlinable
  @_optimize(none)
  public init(literalCapacity: Int, interpolationCount: Int) {
    // Since the format string and the arguments array are fully constructed
    // at compile time, the parameters are ignored.
    formatString = ""
    arguments = OSLogArguments()
    preamble = 0
    argumentCount = 0
    totalBytesForSerializingArguments = 0
  }

  @_semantics("constant_evaluable")
  @inlinable
  @_optimize(none)
  public mutating func appendLiteral(_ literal: String) {
    formatString += literal.percentEscapedString
  }

  /// `appendInterpolation` conformances will be added by extensions to this type.

  /// Return true if and only if the parameter is .private.
  /// This function must be constant evaluable.
  @inlinable
  @_semantics("constant_evaluable")
  @_effects(readonly)
  @_optimize(none)
  internal func isPrivate(_ privacy: Privacy) -> Bool {
    // Do not use equality comparisons on enums as it is not supported by
    // the constant evaluator.
    if case .private = privacy {
      return true
    }
    return false
  }

  /// Compute a byte-sized argument header consisting of flag and type.
  /// Flag and type take up the least and most significant four bits
  /// of the header byte, respectively.
  /// This function should be constant evaluable.
  @inlinable
  @_semantics("constant_evaluable")
  @_effects(readonly)
  @_optimize(none)
  internal func getArgumentHeader(
    isPrivate: Bool,
    type: ArgumentType
  ) -> UInt8 {
    let flag: ArgumentFlag = isPrivate ? .privateFlag : .publicFlag
    let flagAndType: UInt8 = (type.rawValue &<< 4) | flag.rawValue
    return flagAndType
  }

  /// Compute the new preamble based whether the current argument is private
  /// or not. This function must be constant evaluable.
  @inlinable
  @_semantics("constant_evaluable")
  @_effects(readonly)
  @_optimize(none)
  internal func getUpdatedPreamble(
    isPrivate: Bool,
    isScalar: Bool
  ) -> UInt8 {
    var preamble = self.preamble
    if isPrivate {
      preamble |= PreambleBitMask.privateBitMask.rawValue
    }
    if !isScalar {
      preamble |= PreambleBitMask.nonScalarBitMask.rawValue
    }
    return preamble
  }
}

extension String {
  /// Replace all percents "%" in the string by "%%" so that the string can be
  /// interpreted as a C format string. This function is constant evaluable
  /// and its semantics is modeled within the evaluator.
  public var percentEscapedString: String {
    @_semantics("string.escapePercent.get")
    @_effects(readonly)
    @_optimize(none)
    get {
      return self
        .split(separator: "%", omittingEmptySubsequences: false)
        .joined(separator: "%%")
    }
  }
}

@frozen
public struct OSLogMessage :
  ExpressibleByStringInterpolation, ExpressibleByStringLiteral
{
  public let interpolation: OSLogInterpolation

  /// Initializer for accepting string interpolations. This function must be
  /// constant evaluable.
  @inlinable
  @_optimize(none)
  @_semantics("oslog.message.init_interpolation")
  @_semantics("constant_evaluable")
  public init(stringInterpolation: OSLogInterpolation) {
    self.interpolation = stringInterpolation
  }

  /// Initializer for accepting string literals. This function must be
  /// constant evaluable.
  @inlinable
  @_optimize(none)
  @_semantics("oslog.message.init_stringliteral")
  @_semantics("constant_evaluable")
  public init(stringLiteral value: String) {
    var s = OSLogInterpolation(literalCapacity: 1, interpolationCount: 0)
    s.appendLiteral(value)
    self.interpolation = s
  }

  /// The byte size of the buffer that will be passed to the C os_log ABI.
  /// It will contain the elements of `interpolation.arguments` and the two
  /// summary bytes: preamble and argument count.
  @_transparent
  @_optimize(none)
  public var bufferSize: Int {
    return interpolation.totalBytesForSerializingArguments + 2
  }
}

public typealias ByteBufferPointer = UnsafeMutablePointer<UInt8>
public typealias StorageObjects = [AnyObject]

/// A representation of a sequence of arguments and headers (of possibly
/// different types) that have to be serialized to a byte buffer. The arguments
/// are captured within closures and stored in an array. The closures accept an
/// instance of `OSLogByteBufferBuilder`, and when invoked, serialize the
/// argument using the passed `OSLogByteBufferBuilder` instance.
@frozen
@usableFromInline
internal struct OSLogArguments {
  /// An array of closures that captures arguments of possibly different types.
  /// Each closure accepts a pointer into a byte buffer and serializes the
  /// captured arguments at the pointed location. The closures also accept an
  /// array of AnyObject to store references to auxiliary storage created during
  /// serialization.
  @usableFromInline
  internal var argumentClosures: [(inout ByteBufferPointer,
    inout StorageObjects) -> ()]

  @_semantics("constant_evaluable")
  @inlinable
  @_optimize(none)
  internal init() {
    argumentClosures = []
  }

  /// Append a byte-sized header, constructed by
  /// `OSLogMessage.appendInterpolation`, to the tracked array of closures.
  @_semantics("constant_evaluable")
  @inlinable
  @_optimize(none)
  internal mutating func append(_ header: UInt8) {
    argumentClosures.append({ (position, _) in
      serialize(header, at: &position)
    })
  }

  /// `append` for other types must be implemented by extensions.
}

/// Serialize a UInt8 value at the buffer location pointed to by `bufferPosition`,
/// and increment the `bufferPosition` with the byte size of the serialized value.
@usableFromInline
@_alwaysEmitIntoClient
internal func serialize(
  _ value: UInt8,
  at bufferPosition: inout ByteBufferPointer)
{
  bufferPosition[0] = value
  bufferPosition += 1
}
