// Copyright 2023 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#if !V8_ENABLE_WEBASSEMBLY
#error This header should only be included if WebAssembly is enabled.
#endif  // !V8_ENABLE_WEBASSEMBLY

#ifndef V8_COMPILER_TURBOSHAFT_WASM_GC_TYPE_REDUCER_H_
#define V8_COMPILER_TURBOSHAFT_WASM_GC_TYPE_REDUCER_H_

#include "src/compiler/turboshaft/assembler.h"
#include "src/compiler/turboshaft/operations.h"
#include "src/compiler/turboshaft/snapshot-table-opindex.h"
#include "src/compiler/wasm-graph-assembler.h"
#include "src/wasm/wasm-subtyping.h"

namespace v8::internal::compiler::turboshaft {

// The WasmGCTypeReducer infers type information based on the input graph and
// reduces type checks and casts based on that information.
//
// This is done in two steps:
// 1) The WasmGCTypeAnalyzer infers the types based on the input graph, e.g.:
//    func (param anyref) (result i32)
//      local.get 0
//      ref.test $MyType
//      if                     // local 0 is known to be (a subtype of) $MyType
//        local.get 0
//        ref.cast $MyType     // the input of this cast is a subtype of $MyType
//                             // it can be removed during reduction
//        struct.get $MyType 0
//        return
//      end                    // local 0 is still anyref
//        i32.const 0
//
// 2) The WasmGCTypeReducer reduces the graph to a new graph potentially
//    removing, simplifying (e.g. replacing a cast with a null check) or
//    refining (setting the from type to a more specific type) type operations.

class WasmGCTypeAnalyzer {
 public:
  WasmGCTypeAnalyzer(Graph& graph, Zone* zone)
      : graph_(graph), phase_zone_(zone) {}

  void Run();

  wasm::ValueType GetInputType(OpIndex op) const {
    auto iter = input_type_map_.find(op);
    DCHECK_NE(iter, input_type_map_.end());
    return iter->second;
  }

 private:
  using TypeSnapshotTable = SparseOpIndexSnapshotTable<wasm::ValueType>;
  using Snapshot = TypeSnapshotTable::Snapshot;
  using MaybeSnapshot = TypeSnapshotTable::MaybeSnapshot;

  void StartNewSnapshotFor(const Block& block);
  void ProcessOperations(const Block& block);
  void ProcessBranchOnTarget(const BranchOp& branch, const Block& target);

  void ProcessTypeCast(const WasmTypeCastOp& type_cast);
  void ProcessTypeCheck(const WasmTypeCheckOp& type_check);
  void ProcessAssertNotNull(const AssertNotNullOp& type_cast);
  void ProcessNull(const NullOp& null);
  void ProcessIsNull(const IsNullOp& is_null);
  void ProcessParameter(const ParameterOp& parameter);
  void ProcessStructGet(const StructGetOp& struct_get);
  void ProcessStructSet(const StructSetOp& struct_set);
  void ProcessArrayLength(const ArrayLengthOp& array_length);
  void ProcessGlobalGet(const GlobalGetOp& global_get);
  void ProcessRefFunc(const WasmRefFuncOp& ref_func);
  void ProcessAllocateArray(const WasmAllocateArrayOp& allocate_array);
  void ProcessAllocateStruct(const WasmAllocateStructOp& allocate_struct);
  void ProcessPhi(const PhiOp& phi);

  void CreateMergeSnapshot(const Block& block);

  // Updates the knowledge in the side table about the type of {object},
  // returning the previous known type.
  wasm::ValueType RefineTypeKnowledge(OpIndex object, wasm::ValueType new_type);
  // Updates the knowledge in the side table to be a non-nullable type for
  // {object}, returning the previous known type.
  wasm::ValueType RefineTypeKnowledgeNotNull(OpIndex object);

  Graph& graph_;
  Zone* phase_zone_;
  const wasm::WasmModule* module_ = PipelineData::Get().wasm_module();
  const wasm::FunctionSig* signature_ = PipelineData::Get().wasm_sig();
  // Contains the snapshots for all blocks in the CFG.
  TypeSnapshotTable types_table_{phase_zone_};
  // Maps the block id to a snapshot in the table defining the type knowledge
  // at the end of the block.
  FixedBlockSidetable<MaybeSnapshot> block_to_snapshot_{graph_.block_count(),
                                                        phase_zone_};
  // For any operation that could potentially refined, this map stores an entry
  // to the inferred input type based on the analysis.
  ZoneUnorderedMap<OpIndex, wasm::ValueType> input_type_map_{phase_zone_};
  bool is_in_loop_header_ = false;
};

#include "src/compiler/turboshaft/define-assembler-macros.inc"

template <class Next>
class WasmGCTypeReducer : public Next {
 public:
  TURBOSHAFT_REDUCER_BOILERPLATE()

  void Analyze() {
    analyzer_.Run();
    Next::Analyze();
  }

  OpIndex REDUCE_INPUT_GRAPH(WasmTypeCast)(OpIndex op_idx,
                                           const WasmTypeCastOp& cast_op) {
    wasm::ValueType type = analyzer_.GetInputType(op_idx);
    if (type != wasm::ValueType() && type != wasm::kWasmBottom) {
      bool to_nullable = cast_op.config.to.is_nullable();
      if (wasm::IsHeapSubtypeOf(type.heap_type(), cast_op.config.to.heap_type(),
                                module_, module_)) {
        if (to_nullable || type.is_non_nullable()) {
          // The inferred type is already as specific as the cast target, the
          // cast is guaranteed to always succeed and can therefore be removed.
          return __ MapToNewGraph(cast_op.object());
        } else {
          // The inferred heap type is already as specific as the cast target,
          // but the source can be nullable and the target cannot be, so a null
          // check is still required.
          return __ AssertNotNull(__ MapToNewGraph(cast_op.object()), type,
                                  TrapId::kTrapIllegalCast);
        }
      }
      if (wasm::HeapTypesUnrelated(type.heap_type(),
                                   cast_op.config.to.heap_type(), module_,
                                   module_) &&
          !wasm::IsImplicitInternalization(type, cast_op.config.to, module_)) {
        // A cast between unrelated types can only succeed if the argument is
        // null. Otherwise, it always fails.
        V<Word32> non_trapping_condition =
            type.is_nullable() && to_nullable ? __ IsNull(__ MapToNewGraph(
                                                              cast_op.object()),
                                                          type)
                                              : __ Word32Constant(0);
        __ TrapIfNot(non_trapping_condition, OpIndex::Invalid(),
                     TrapId::kTrapIllegalCast);
        return __ MapToNewGraph(cast_op.object());
      }
      // The cast cannot be replaced. Still, we can refine the source type, so
      // that the lowering could potentially skip null or smi checks.
      wasm::ValueType from_type =
          wasm::Intersection(type, cast_op.config.from, module_, module_).type;
      DCHECK_NE(wasm::kWasmBottom, from_type);
      WasmTypeCheckConfig config{from_type, cast_op.config.to};
      return __ WasmTypeCast(__ MapToNewGraph(cast_op.object()),
                             __ MapToNewGraphIfValid(cast_op.rtt()), config);
    }
    return Next::ReduceInputGraphWasmTypeCast(op_idx, cast_op);
  }

  OpIndex REDUCE_INPUT_GRAPH(WasmTypeCheck)(OpIndex op_idx,
                                            const WasmTypeCheckOp& type_check) {
    wasm::ValueType type = analyzer_.GetInputType(op_idx);
    if (type != wasm::ValueType() && type != wasm::kWasmBottom) {
      bool to_nullable = type_check.config.to.is_nullable();
      if (wasm::IsHeapSubtypeOf(type.heap_type(),
                                type_check.config.to.heap_type(), module_,
                                module_)) {
        if (to_nullable || type.is_non_nullable()) {
          // The inferred type is guaranteed to be a subtype of the checked
          // type.
          return __ Word32Constant(1);
        } else {
          // The inferred type is guaranteed to be a subtype of the checked
          // type if it is not null.
          return __ Word32Equal(
              __ IsNull(__ MapToNewGraph(type_check.object()), type), 0);
        }
      }
      if (wasm::HeapTypesUnrelated(type.heap_type(),
                                   type_check.config.to.heap_type(), module_,
                                   module_)) {
        if (to_nullable && type.is_nullable()) {
          return __ IsNull(__ MapToNewGraph(type_check.object()), type);
        } else {
          return __ Word32Constant(0);
        }
      }
      // The check cannot be replaced. Still, we can refine the source type, so
      // that the lowering could potentially skip null or smi checks.
      wasm::ValueType from_type =
          wasm::Intersection(type, type_check.config.from, module_, module_)
              .type;
      DCHECK_NE(wasm::kWasmBottom, from_type);
      WasmTypeCheckConfig config{from_type, type_check.config.to};
      return __ WasmTypeCheck(__ MapToNewGraph(type_check.object()),
                              __ MapToNewGraphIfValid(type_check.rtt()),
                              config);
    }
    return Next::ReduceInputGraphWasmTypeCheck(op_idx, type_check);
  }

  OpIndex REDUCE_INPUT_GRAPH(AssertNotNull)(
      OpIndex op_idx, const AssertNotNullOp& assert_not_null) {
    wasm::ValueType type = analyzer_.GetInputType(op_idx);
    if (type.is_non_nullable()) {
      return __ MapToNewGraph(assert_not_null.object());
    }
    return Next::ReduceInputGraphAssertNotNull(op_idx, assert_not_null);
  }

  OpIndex REDUCE_INPUT_GRAPH(IsNull)(OpIndex op_idx, const IsNullOp& is_null) {
    const wasm::ValueType type = analyzer_.GetInputType(op_idx);
    if (type.is_non_nullable()) {
      return __ Word32Constant(0);
    }
    if (type != wasm::ValueType() && type != wasm::kWasmBottom &&
        wasm::ToNullSentinel({type, module_}) == type) {
      return __ Word32Constant(1);
    }
    return Next::ReduceInputGraphIsNull(op_idx, is_null);
  }

  OpIndex REDUCE_INPUT_GRAPH(StructGet)(OpIndex op_idx,
                                        const StructGetOp& struct_get) {
    const wasm::ValueType type = analyzer_.GetInputType(op_idx);
    // Remove the null check if it is known to be not null.
    if (struct_get.null_check == kWithNullCheck && type.is_non_nullable()) {
      return __ StructGet(__ MapToNewGraph(struct_get.object()),
                          struct_get.type, struct_get.field_index,
                          struct_get.is_signed, kWithoutNullCheck);
    }
    return Next::ReduceInputGraphStructGet(op_idx, struct_get);
  }

  OpIndex REDUCE_INPUT_GRAPH(StructSet)(OpIndex op_idx,
                                        const StructSetOp& struct_set) {
    const wasm::ValueType type = analyzer_.GetInputType(op_idx);
    // Remove the null check if it is known to be not null.
    if (struct_set.null_check == kWithNullCheck && type.is_non_nullable()) {
      __ StructSet(__ MapToNewGraph(struct_set.object()),
                   __ MapToNewGraph(struct_set.value()), struct_set.type,
                   struct_set.field_index, kWithoutNullCheck);
      return OpIndex::Invalid();
    }
    return Next::ReduceInputGraphStructSet(op_idx, struct_set);
  }

  OpIndex REDUCE_INPUT_GRAPH(ArrayLength)(OpIndex op_idx,
                                          const ArrayLengthOp& array_length) {
    const wasm::ValueType type = analyzer_.GetInputType(op_idx);
    // Remove the null check if it is known to be not null.
    if (array_length.null_check == kWithNullCheck && type.is_non_nullable()) {
      return __ ArrayLength(__ MapToNewGraph(array_length.array()),
                            kWithoutNullCheck);
    }
    return Next::ReduceInputGraphArrayLength(op_idx, array_length);
  }

  // TODO(14108): This isn't a type optimization and doesn't fit well into this
  // reducer.
  OpIndex REDUCE(AnyConvertExtern)(V<Tagged> object) {
    if (object.valid()) {
      const ExternConvertAnyOp* externalize =
          __ output_graph().Get(object).template TryCast<ExternConvertAnyOp>();
      if (externalize != nullptr) {
        // Directly return the object as
        // any.convert_extern(extern.convert_any(x)) == x.
        return externalize->object();
      }
    }
    return Next::ReduceAnyConvertExtern(object);
  }

 private:
  Graph& graph_ = __ modifiable_input_graph();
  const wasm::WasmModule* module_ = PipelineData::Get().wasm_module();
  WasmGCTypeAnalyzer analyzer_{graph_, __ phase_zone()};
};

#include "src/compiler/turboshaft/undef-assembler-macros.inc"

}  // namespace v8::internal::compiler::turboshaft

#endif  // V8_COMPILER_TURBOSHAFT_WASM_GC_TYPE_REDUCER_H_
