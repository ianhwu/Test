//===--- main.swift -------------------------------------------*- swift -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2018 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

// This is just a driver for performance overview tests.

import TestsUtils
import DriverUtils
import Ackermann
import AngryPhonebook
import AnyHashableWithAClass
import Array2D
import ArrayAppend
import ArrayInClass
import ArrayLiteral
import ArrayOfGenericPOD
import ArrayOfGenericRef
import ArrayOfPOD
import ArrayOfRef
import ArraySetElement
import ArraySubscript
import BinaryFloatingPointConversionFromBinaryInteger
import BinaryFloatingPointProperties
import BitCount
import Breadcrumbs
import BucketSort
import ByteSwap
import COWTree
import COWArrayGuaranteedParameterOverhead
import CString
import CSVParsing
import Calculator
import CaptureProp
import ChaCha
import ChainedFilterMap
import CharacterLiteralsLarge
import CharacterLiteralsSmall
import CharacterProperties
import Chars
import ClassArrayGetter
import Codable
import Combos
import DataBenchmarks
import DeadArray
import DictOfArraysToArrayOfDicts
import DictTest
import DictTest2
import DictTest3
import DictTest4
import DictTest4Legacy
import DictionaryBridge
import DictionaryBridgeToObjC
import DictionaryCompactMapValues
import DictionaryCopy
import DictionaryGroup
import DictionaryKeysContains
import DictionaryLiteral
import DictionaryOfAnyHashableStrings
import DictionaryRemove
import DictionarySubscriptDefault
import DictionarySwap
import Diffing
import DiffingMyers
import DropFirst
import DropLast
import DropWhile
import ErrorHandling
import Exclusivity
import ExistentialPerformance
import Fibonacci
import FindStringNaive
import FlattenList
import FloatingPointParsing
import FloatingPointPrinting
import Hanoi
import Hash
import Histogram
import InsertCharacter
import IntegerParsing
import Integrate
import IterateData
import Join
import LazyFilter
import LinkedList
import LuhnAlgoEager
import LuhnAlgoLazy
import MapReduce
import Memset
import MonteCarloE
import MonteCarloPi
import NibbleSort
import NIOChannelPipeline
import NSDictionaryCastToSwift
import NSError
#if os(macOS) || os(iOS) || os(watchOS) || os(tvOS)
import NSStringConversion
#endif
import NopDeinit
import ObjectAllocation
#if os(macOS) || os(iOS) || os(watchOS) || os(tvOS)
import ObjectiveCBridging
import ObjectiveCBridgingStubs
#if !(SWIFT_PACKAGE || Xcode)
import ObjectiveCNoBridgingStubs
#endif
#endif
import ObserverClosure
import ObserverForwarderStruct
import ObserverPartiallyAppliedMethod
import ObserverUnappliedMethod
import OpaqueConsumingUsers
import OpenClose
import Phonebook
import PointerArithmetics
import PolymorphicCalls
import PopFront
import PopFrontGeneric
import Prefix
import PrefixWhile
import Prims
import PrimsNonStrongRef
import PrimsSplit
import ProtocolDispatch
import ProtocolDispatch2
import Queue
import RC4
import RGBHistogram
import Radix2CooleyTukey
import RandomShuffle
import RandomValues
import RangeAssignment
import RangeIteration
import RangeOverlaps
import RangeReplaceableCollectionPlusDefault
import RecursiveOwnedParameter
import ReduceInto
import RemoveWhere
import ReversedCollections
import RomanNumbers
import SequenceAlgos
import SetTests
import SevenBoom
import Sim2DArray
import SortArrayInClass
import SortIntPyramids
import SortLargeExistentials
import SortLettersInPlace
import SortStrings
import StackPromo
import StaticArray
import StrComplexWalk
import StrToInt
import StringBuilder
import StringComparison
import StringEdits
import StringEnum
import StringInterpolation
import StringMatch
import StringRemoveDupes
import StringReplaceSubrange
import StringTests
import StringWalk
import Substring
import Suffix
import SuperChars
import TwoSum
import TypeFlood
import UTF8Decode
import Walsh
import WordCount
import XorLoop

@inline(__always)
private func registerBenchmark(_ bench: BenchmarkInfo) {
  registeredBenchmarks.append(bench)
}

@inline(__always)
private func registerBenchmark<
  S : Sequence
>(_ infos: S) where S.Element == BenchmarkInfo {
  registeredBenchmarks.append(contentsOf: infos)
}

registerBenchmark(Ackermann)
registerBenchmark(AngryPhonebook)
registerBenchmark(AnyHashableWithAClass)
registerBenchmark(Array2D)
registerBenchmark(ArrayAppend)
registerBenchmark(ArrayInClass)
registerBenchmark(ArrayLiteral)
registerBenchmark(ArrayOfGenericPOD)
registerBenchmark(ArrayOfGenericRef)
registerBenchmark(ArrayOfPOD)
registerBenchmark(ArrayOfRef)
registerBenchmark(ArraySetElement)
registerBenchmark(ArraySubscript)
registerBenchmark(BinaryFloatingPointConversionFromBinaryInteger)
registerBenchmark(BinaryFloatingPointPropertiesBinade)
registerBenchmark(BinaryFloatingPointPropertiesNextUp)
registerBenchmark(BinaryFloatingPointPropertiesUlp)
registerBenchmark(BitCount)
registerBenchmark(Breadcrumbs)
registerBenchmark(BucketSort)
registerBenchmark(ByteSwap)
registerBenchmark(COWTree)
registerBenchmark(COWArrayGuaranteedParameterOverhead)
registerBenchmark(CString)
registerBenchmark(CSVParsing)
registerBenchmark(Calculator)
registerBenchmark(CaptureProp)
registerBenchmark(ChaCha)
registerBenchmark(ChainedFilterMap)
registerBenchmark(CharacterLiteralsLarge)
registerBenchmark(CharacterLiteralsSmall)
registerBenchmark(CharacterPropertiesFetch)
registerBenchmark(CharacterPropertiesStashed)
registerBenchmark(CharacterPropertiesStashedMemo)
registerBenchmark(CharacterPropertiesPrecomputed)
registerBenchmark(Chars)
registerBenchmark(Codable)
registerBenchmark(Combos)
registerBenchmark(ClassArrayGetter)
registerBenchmark(DataBenchmarks)
registerBenchmark(DeadArray)
registerBenchmark(DictOfArraysToArrayOfDicts)
registerBenchmark(Dictionary)
registerBenchmark(Dictionary2)
registerBenchmark(Dictionary3)
registerBenchmark(Dictionary4)
registerBenchmark(Dictionary4Legacy)
registerBenchmark(DictionaryBridge)
registerBenchmark(DictionaryBridgeToObjC)
registerBenchmark(DictionaryCompactMapValues)
registerBenchmark(DictionaryCopy)
registerBenchmark(DictionaryGroup)
registerBenchmark(DictionaryKeysContains)
registerBenchmark(DictionaryLiteral)
registerBenchmark(DictionaryOfAnyHashableStrings)
registerBenchmark(DictionaryRemove)
registerBenchmark(DictionarySubscriptDefault)
registerBenchmark(DictionarySwap)
registerBenchmark(Diffing)
registerBenchmark(DiffingMyers)
registerBenchmark(DropFirst)
registerBenchmark(DropLast)
registerBenchmark(DropWhile)
registerBenchmark(ErrorHandling)
registerBenchmark(Exclusivity)
registerBenchmark(ExistentialPerformance)
registerBenchmark(Fibonacci)
registerBenchmark(FindStringNaive)
registerBenchmark(FlattenListLoop)
registerBenchmark(FlattenListFlatMap)
registerBenchmark(FloatingPointParsing)
registerBenchmark(FloatingPointPrinting)
registerBenchmark(Hanoi)
registerBenchmark(HashTest)
registerBenchmark(Histogram)
registerBenchmark(InsertCharacter)
registerBenchmark(IntegerParsing)
registerBenchmark(IntegrateTest)
registerBenchmark(IterateData)
registerBenchmark(Join)
registerBenchmark(LazyFilter)
registerBenchmark(LinkedList)
registerBenchmark(LuhnAlgoEager)
registerBenchmark(LuhnAlgoLazy)
registerBenchmark(MapReduce)
registerBenchmark(Memset)
registerBenchmark(MonteCarloE)
registerBenchmark(MonteCarloPi)
registerBenchmark(NSDictionaryCastToSwift)
registerBenchmark(NSErrorTest)
#if os(macOS) || os(iOS) || os(watchOS) || os(tvOS)
registerBenchmark(NSStringConversion)
#endif
registerBenchmark(NibbleSort)
registerBenchmark(NIOChannelPipeline)
registerBenchmark(NopDeinit)
registerBenchmark(ObjectAllocation)
#if os(macOS) || os(iOS) || os(watchOS) || os(tvOS)
registerBenchmark(ObjectiveCBridging)
registerBenchmark(ObjectiveCBridgingStubs)
#if !(SWIFT_PACKAGE || Xcode)
registerBenchmark(ObjectiveCNoBridgingStubs)
#endif
#endif
registerBenchmark(ObserverClosure)
registerBenchmark(ObserverForwarderStruct)
registerBenchmark(ObserverPartiallyAppliedMethod)
registerBenchmark(ObserverUnappliedMethod)
registerBenchmark(OpaqueConsumingUsers)
registerBenchmark(OpenClose)
registerBenchmark(Phonebook)
registerBenchmark(PointerArithmetics)
registerBenchmark(PolymorphicCalls)
registerBenchmark(PopFront)
registerBenchmark(PopFrontArrayGeneric)
registerBenchmark(Prefix)
registerBenchmark(PrefixWhile)
registerBenchmark(Prims)
registerBenchmark(PrimsNonStrongRef)
registerBenchmark(PrimsSplit)
registerBenchmark(ProtocolDispatch)
registerBenchmark(ProtocolDispatch2)
registerBenchmark(QueueGeneric)
registerBenchmark(QueueConcrete)
registerBenchmark(RC4Test)
registerBenchmark(RGBHistogram)
registerBenchmark(Radix2CooleyTukey)
registerBenchmark(RandomShuffle)
registerBenchmark(RandomValues)
registerBenchmark(RangeAssignment)
registerBenchmark(RangeIteration)
registerBenchmark(RangeOverlaps)
registerBenchmark(RangeReplaceableCollectionPlusDefault)
registerBenchmark(RecursiveOwnedParameter)
registerBenchmark(ReduceInto)
registerBenchmark(RemoveWhere)
registerBenchmark(ReversedCollections)
registerBenchmark(RomanNumbers)
registerBenchmark(SequenceAlgos)
registerBenchmark(SetTests)
registerBenchmark(SevenBoom)
registerBenchmark(Sim2DArray)
registerBenchmark(SortArrayInClass)
registerBenchmark(SortIntPyramids)
registerBenchmark(SortLargeExistentials)
registerBenchmark(SortLettersInPlace)
registerBenchmark(SortStrings)
registerBenchmark(StackPromo)
registerBenchmark(StaticArrayTest)
registerBenchmark(StrComplexWalk)
registerBenchmark(StrToInt)
registerBenchmark(StringBuilder)
registerBenchmark(StringComparison)
registerBenchmark(StringEdits)
registerBenchmark(StringEnum)
registerBenchmark(StringHashing)
registerBenchmark(StringInterpolation)
registerBenchmark(StringInterpolationSmall)
registerBenchmark(StringInterpolationManySmallSegments)
registerBenchmark(StringMatch)
registerBenchmark(StringNormalization)
registerBenchmark(StringRemoveDupes)
registerBenchmark(StringReplaceSubrange)
registerBenchmark(StringTests)
registerBenchmark(StringWalk)
registerBenchmark(SubstringTest)
registerBenchmark(Suffix)
registerBenchmark(SuperChars)
registerBenchmark(TwoSum)
registerBenchmark(TypeFlood)
registerBenchmark(UTF8Decode)
registerBenchmark(Walsh)
registerBenchmark(WordCount)
registerBenchmark(XorLoop)

main()
