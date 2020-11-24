//===--- FoundationHelpers.mm - Cocoa framework helper shims --------------===//
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
// This file contains shims to refer to framework functions required by the
// standard library. The stdlib cannot directly import these modules without
// introducing circular dependencies.
//
//===----------------------------------------------------------------------===//

#include "swift/Runtime/Config.h"

#if SWIFT_OBJC_INTEROP
#import <CoreFoundation/CoreFoundation.h>
#include "../SwiftShims/CoreFoundationShims.h"
#import <objc/runtime.h>

using namespace swift;

template <class FromTy> struct DestType;

#define BRIDGE_TYPE(FROM, TO) \
template <> struct DestType<FROM> { using type = TO; }

BRIDGE_TYPE(_swift_shims_CFAllocatorRef, CFAllocatorRef);
BRIDGE_TYPE(_swift_shims_CFStringRef, CFStringRef);
BRIDGE_TYPE(_swift_shims_UniChar *, UniChar *);
BRIDGE_TYPE(_swift_shims_CFStringEncoding, CFStringEncoding);
BRIDGE_TYPE(_swift_shims_CFStringCompareFlags, CFStringCompareFlags);
BRIDGE_TYPE(_swift_shims_CFRange *, CFRange *);
BRIDGE_TYPE(CFComparisonResult, _swift_shims_CFComparisonResult);
BRIDGE_TYPE(CFStringRef, _swift_shims_CFStringRef);

template <class FromTy>
static typename DestType<FromTy>::type cast(FromTy value) {
  return (typename DestType<FromTy>::type) value;
}

static CFRange cast(_swift_shims_CFRange value) {
  return { value.location, value.length };
}

void swift::_swift_stdlib_CFStringGetCharacters(
                                         _swift_shims_CFStringRef theString,
                                         _swift_shims_CFRange range,
                                         _swift_shims_UniChar *buffer) {
  return CFStringGetCharacters(cast(theString), cast(range), cast(buffer));
}

const _swift_shims_UniChar *
swift::_swift_stdlib_CFStringGetCharactersPtr(
                                         _swift_shims_CFStringRef theString) {
  return CFStringGetCharactersPtr(cast(theString));
}

_swift_shims_CFIndex swift::_swift_stdlib_CFStringGetBytes(
    _swift_shims_CFStringRef theString, _swift_shims_CFRange range,
    _swift_shims_CFStringEncoding encoding, _swift_shims_UInt8 lossByte,
    _swift_shims_Boolean isExternalRepresentation, _swift_shims_UInt8 *buffer,
    _swift_shims_CFIndex maxBufLen, _swift_shims_CFIndex *usedBufLen) {
  return CFStringGetBytes(cast(theString), cast(range), encoding, lossByte,
                          isExternalRepresentation, buffer, maxBufLen, usedBufLen);
}

_swift_shims_CFIndex
swift::_swift_stdlib_CFStringGetLength(_swift_shims_CFStringRef theString) {
  return CFStringGetLength(cast(theString));
}

_swift_shims_CFStringRef
swift::_swift_stdlib_CFStringCreateWithSubstring(
                                         const void *unused,
                                         _swift_shims_CFStringRef str,
                                         _swift_shims_CFRange range) {
  assert(unused == NULL);
  return cast(CFStringCreateWithSubstring(kCFAllocatorSystemDefault,
                                          cast(str),
                                          cast(range)));
}

_swift_shims_CFComparisonResult
swift::_swift_stdlib_CFStringCompare(
                              _swift_shims_CFStringRef string,
                              _swift_shims_CFStringRef string2) {
  return cast(CFStringCompareWithOptionsAndLocale(cast(string),
                                                  cast(string2),
                                                  { 0, CFStringGetLength(cast(string)) },
                                                  0,
                                                  NULL));
}

__swift_uint8_t
swift::_swift_stdlib_isNSString(id obj) {
  //TODO: we can likely get a small perf win by using _NSIsNSString on
  //sufficiently new OSs
  return CFGetTypeID((CFTypeRef)obj) == CFStringGetTypeID() ? 1 : 0;
}

_swift_shims_UniChar
swift::_swift_stdlib_CFStringGetCharacterAtIndex(_swift_shims_CFStringRef theString,
                                                 _swift_shims_CFIndex idx) {
  return CFStringGetCharacterAtIndex(cast(theString), idx);
}

_swift_shims_CFStringRef
swift::_swift_stdlib_CFStringCreateCopy(const void *unused,
                                        _swift_shims_CFStringRef theString) {
  assert(unused == NULL);
  return cast(CFStringCreateCopy(kCFAllocatorSystemDefault, cast(theString)));
}

_swift_shims_CFStringRef
swift::_swift_stdlib_CFStringCreateWithBytes(
    const void *unused, const uint8_t *bytes,
    _swift_shims_CFIndex numBytes, _swift_shims_CFStringEncoding encoding,
    _swift_shims_Boolean isExternalRepresentation) {
  assert(unused == NULL);
  return cast(CFStringCreateWithBytes(kCFAllocatorSystemDefault, bytes, numBytes,
                                      cast(encoding),
                                      isExternalRepresentation));
}

const char *
swift::_swift_stdlib_CFStringGetCStringPtr(_swift_shims_CFStringRef theString,
                            _swift_shims_CFStringEncoding encoding) {
  return CFStringGetCStringPtr(cast(theString), cast(encoding));
}

_swift_shims_CFStringRef
swift::_swift_stdlib_objcDebugDescription(id _Nonnull nsObject) {
  return [nsObject debugDescription];
}

extern "C" CFHashCode CFStringHashCString(const uint8_t *bytes, CFIndex len);
extern "C" CFHashCode CFStringHashNSString(id str);


_swift_shims_CFHashCode
swift::_swift_stdlib_CFStringHashNSString(id _Nonnull obj) {
  return CFStringHashNSString(obj);
}

_swift_shims_CFHashCode
swift::_swift_stdlib_CFStringHashCString(const _swift_shims_UInt8 * _Nonnull bytes,
                                  _swift_shims_CFIndex length) {
  return CFStringHashCString(bytes, length);
}

const __swift_uint8_t *
swift::_swift_stdlib_NSStringCStringUsingEncodingTrampoline(id _Nonnull obj,
                                                  unsigned long encoding) {
  typedef __swift_uint8_t * _Nullable (*cStrImplPtr)(id, SEL, unsigned long);
  cStrImplPtr imp = (cStrImplPtr)class_getMethodImplementation([obj superclass],
                                                               @selector(cStringUsingEncoding:));
  return imp(obj, @selector(cStringUsingEncoding:), encoding);
}

__swift_uint8_t
swift::_swift_stdlib_NSStringGetCStringTrampoline(id _Nonnull obj,
                                         _swift_shims_UInt8 *buffer,
                                         _swift_shims_CFIndex maxLength,
                                         unsigned long encoding) {
  typedef __swift_uint8_t (*getCStringImplPtr)(id,
                                             SEL,
                                             _swift_shims_UInt8 *,
                                             _swift_shims_CFIndex,
                                             unsigned long);
  SEL sel = @selector(getCString:maxLength:encoding:);
  getCStringImplPtr imp = (getCStringImplPtr)class_getMethodImplementation([obj superclass], sel);
  
  return imp(obj, sel, buffer, maxLength, encoding);

}

__swift_uintptr_t
swift::_swift_stdlib_unsafeAddressOfClass(id _Nonnull obj) {
  return (__swift_uintptr_t)object_getClass(obj); //TODO: do direct isa access when in the OS
}


#endif

