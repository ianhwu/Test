//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2018 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

/// NWProtocolDefinition is an abstract superclass that represents the identifier of a
/// protocol that can be used with connections and listeners, such as TCP.
// NOTE: older overlays had Network.NWProtocolDefinition as the ObjC name.
// The two must coexist, so it was renamed. The old name must not be
// used in the new runtime. _TtC7Network21_NWProtocolDefinition is the
// mangled name for Network._NWProtocolDefinition.
@_objcRuntimeName(_TtC7Network21_NWProtocolDefinition)
@available(macOS 10.14, iOS 12.0, watchOS 5.0, tvOS 12.0, *)
public class NWProtocolDefinition : Equatable, CustomDebugStringConvertible {
	public static func == (lhs: NWProtocolDefinition, rhs: NWProtocolDefinition) -> Bool {
		return nw_protocol_definition_is_equal(lhs.nw, rhs.nw)
	}

	/// The name of the protocol, such as "TCP" or "UDP"
	public let name: String
	internal let nw: nw_protocol_definition_t

	internal init(_ nw: nw_protocol_definition_t, _ name: String) {
		self.name = name
		self.nw = nw
	}

	public var debugDescription: String {
		return self.name
	}
}

/// NWProtocolOptions is an abstract superclass that represents a configuration options
/// that can be used to add a protocol into an NWParameters.ProtocolStack. These options
/// configure the behavior of a protocol and cannot be changed after starting a connection.
// NOTE: older overlays had Network.NWProtocolOptions as the ObjC name.
// The two must coexist, so it was renamed. The old name must not be
// used in the new runtime. _TtC7Network18_NWProtocolOptions is the
// mangled name for Network._NWProtocolOptions.
@_objcRuntimeName(_TtC7Network18_NWProtocolOptions)
@available(macOS 10.14, iOS 12.0, watchOS 5.0, tvOS 12.0, *)
public class NWProtocolOptions {
	internal let nw: nw_protocol_options_t


	internal init(_ nw: nw_protocol_options_t) {
		self.nw = nw
	}
}

/// NWProtocolMetadata is an abstract superclass. An instance of metadata holds a set of
/// protocol-specific metadata. This metadata allows clients to pass down protocol requirements
/// specific to some content being sent; as well as to retrieve metadata specific to some
/// content that was received. Each protocol is responsible for defining its own accessor
/// functions to set and get metadata.
// NOTE: older overlays had Network.NWProtocolMetadata as the ObjC name.
// The two must coexist, so it was renamed. The old name must not be
// used in the new runtime. _TtC7Network19_NWProtocolMetadata is the
// mangled name for Network._NWProtocolMetadata.
@_objcRuntimeName(_TtC7Network19_NWProtocolMetadata)
@available(macOS 10.14, iOS 12.0, watchOS 5.0, tvOS 12.0, *)
public class NWProtocolMetadata {
	internal let nw: nw_protocol_metadata_t

	internal init(_ nw: nw_protocol_metadata_t) {
		self.nw = nw
	}
}

/// NWProtocol is an abstract superclass to which protocol implementations conform.
// NOTE: older overlays had Network.NWProtocol as the ObjC name.
// The two must coexist, so it was renamed. The old name must not be
// used in the new runtime. _TtC7Network11_NWProtocol is the
// mangled name for Network._NWProtocol.
@_objcRuntimeName(_TtC7Network11_NWProtocol)
@available(macOS 10.14, iOS 12.0, watchOS 5.0, tvOS 12.0, *)
public class NWProtocol {

}
