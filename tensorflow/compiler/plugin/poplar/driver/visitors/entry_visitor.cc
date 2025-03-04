/* Copyright 2018 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tensorflow/compiler/plugin/poplar/driver/visitors/entry_visitor.h"

#include <queue>
#include <string>
#include <vector>

#include <poplar/ReplicatedStreamMode.hpp>

#include "tensorflow/compiler/plugin/poplar/driver/compiler_resources.h"
#include "tensorflow/compiler/plugin/poplar/driver/ops/ops.h"
#include "tensorflow/compiler/plugin/poplar/driver/poplar_executor.h"
#include "tensorflow/compiler/plugin/poplar/driver/tensor.h"
#include "tensorflow/compiler/plugin/poplar/driver/tools/data_initializer.h"
#include "tensorflow/compiler/plugin/poplar/driver/tools/util.h"

namespace xla {
namespace poplarplugin {
namespace {
// Helper function which marks all the entry computation inputs as unallocated.
DeferredArgRBVectors MakeArgRBVector(const HloComputation* comp) {
  DeferredArgRBVectors output(comp->num_parameters());
  for (int64 i = 0; i != comp->num_parameters(); ++i) {
    output[i].resize(
        FlattenedXlaShape(comp->parameter_instruction(i)->shape()).size());
  }
  return output;
}

Status AddHostToDeviceCopy(const poplar::DataStream& stream, poplar::Tensor dst,
                           bool rearrange_on_host, poplar::Graph& graph,
                           CompilerResources& res,
                           poplar::program::Sequence& seq,
                           const InputOutputAliasingMap::InputInfo& info,
                           const HloInstruction* inst,
                           const poplar::DebugNameAndId& debug_name_and_id) {
  if (res.use_verified_transfers) {
    if (rearrange_on_host) {
      LOG(WARNING)
          << "rearrange_on_host cannot be used with verified streams: ignored.";
    }
    TF_ASSIGN_OR_RETURN(poplar::Tensor index,
                        res.streams_indices.IndexTensor(info, inst, seq));
    seq.add(poplar::program::Copy(stream, dst, index, false,
                                  res.streams_indices.CopyOptions(),
                                  debug_name_and_id));
  } else {
    seq.add(poplar::program::Copy(stream, dst, rearrange_on_host,
                                  debug_name_and_id));
  }
  return Status::OK();
}

Status AddDeviceToHostCopy(const poplar::Tensor src, poplar::DataStream& stream,
                           bool rearrange_on_host, poplar::Graph& graph,
                           CompilerResources& res,
                           poplar::program::Sequence& seq,
                           const InputOutputAliasingMap::OutputInfo& info,
                           const HloInstruction* inst,
                           const poplar::DebugNameAndId& debug_name_and_id) {
  if (res.use_verified_transfers) {
    if (rearrange_on_host) {
      LOG(WARNING)
          << "rearrange_on_host cannot be used with verified streams: ignored.";
    }
    TF_ASSIGN_OR_RETURN(poplar::Tensor index,
                        res.streams_indices.IndexTensor(info, inst, seq));
    seq.add(poplar::program::Copy(src, stream, index, false,
                                  res.streams_indices.CopyOptions(),
                                  debug_name_and_id));
  } else {
    seq.add(poplar::program::Copy(src, stream, rearrange_on_host,
                                  debug_name_and_id));
  }
  return Status::OK();
}

Status CheckNoOpaqueTypes(const HloInstruction* root) {
  auto is_opaque =
      [](const xla::ShapeUtil::IndexedShape& indexed_shape) -> bool {
    return indexed_shape.shape.IsOpaque();
  };
  bool has_opaque =
      absl::c_any_of(ShapeUtil::GetLeafShapes(root->shape()), is_opaque);

  if (has_opaque) {
    return xla::FailedPrecondition(
        "Cannot return or take as an argument an opaque type in the entry "
        "computation. This can be the happen when an IPU specific operation "
        "(e.g. dropout) is used outside a compilation scope. Make sure to use "
        "tf.function, ipu_compiler.compile, or don't use the IPU specific "
        "operators.");
  }

  return Status::OK();
}
}  // namespace

EntryVisitor::EntryVisitor(CompilerResources& resources,
                           const HloComputation* comp)
    : DeferredVisitor(resources, MakeArgRBVector(comp), "Entry"),
      host_to_device({}, dnai_),
      device_to_host({}, dnai_) {}

Status EntryVisitor::AddSequenceForInstruction(
    const HloInstruction* inst, const poplar::program::Sequence& seq) {
  // Use the right sequence for stream copies, otherwise fallback to the
  // default.
  if (inst->opcode() == HloOpcode::kParameter) {
    const auto& in_info = resources_.annotations.input_output_aliasing_map
                              .GetEntryInputInfos()[inst->parameter_number()];
    if (!in_info.IsStreaming()) {
      host_to_device.add(seq);
      return Status::OK();
    }
  }

  return DeferredVisitor::AddSequenceForInstruction(inst, seq);
}

Status EntryVisitor::PreProcessParameter(HloInstruction* parameter) {
  TF_RETURN_IF_ERROR(CheckNoOpaqueTypes(parameter));

  return Status::OK();
}

StatusOr<poplar::Tensor> EntryVisitor::PostProcessParameterAllocation(
    TensorLocation location, const Shape& shape,
    poplar::program::Sequence& stream_copy_seq, poplar::Tensor tensor,
    const poplar::DebugNameAndId& debug_name_and_id) {
  const HloInstruction* inst = location.instruction;
  const int64 flat_tuple_index = location.flattened_output_tuple_index;

  const auto& in_info = resources_.annotations.input_output_aliasing_map
                            .GetEntryInputInfos()[inst->parameter_number()];

  std::vector<Shape> module_shapes;

  const HloModule* module = inst->GetModule();
  const ComputationLayout layout = module->entry_computation_layout();
  if (layout.parameter_count() > inst->parameter_number()) {
    const Shape& mod_shape = layout.parameter_shape(inst->parameter_number());
    module_shapes = FlattenedXlaShape(mod_shape);
  }

  poplar::Graph& graph = GetGraph(resources_, inst);

  const auto use_synthetic_data =
      UseSyntheticDataFor(SyntheticDataCategory::Parameters);
  if (!use_synthetic_data) {
    poplar::Tensor tensor_destination = tensor;
    if (!LayoutUtil::IsMonotonicWithDim0Major(
            module_shapes[flat_tuple_index].layout())) {
      // Host tensor needs to be host layout.
      tensor_destination =
          ConvertFromDeviceLayout(module_shapes[flat_tuple_index], tensor);
    }

    // Create a host stream.
    const std::string handle = in_info.Handles().at(flat_tuple_index);
    auto fifo = graph.addHostToDeviceFIFO(
        handle, tensor_destination.elementType(),
        tensor_destination.numElements(),
        poplar::ReplicatedStreamMode::BROADCAST,
        resources_.streams_indices.GraphOptions(handle));

    TF_RETURN_IF_ERROR(AddHostToDeviceCopy(
        fifo, tensor_destination,
        !in_info.IsStreaming() || resources_.always_rearrange_copies_on_host,
        graph, resources_, stream_copy_seq, in_info, inst, debug_name_and_id));

    InputInfo info = {in_info.Name(), handle, inst->parameter_number(),
                      flat_tuple_index, inst->shape()};
    TF_RETURN_IF_ERROR(AddEntryInputInfo(resources_.annotations, info));
  } else if (use_synthetic_data && UseSyntheticDataInitializer()) {
    // Initialize the tensor to a constant value.
    auto& initializer = DataInitializer::GetSyntheticDataInitializer();
    TF_ASSIGN_OR_RETURN(auto literal, initializer.GetData(shape));
    TF_RETURN_IF_ERROR(SetInitialTensorValue(graph, tensor, literal));
  }

  // If a the input to the graph is a resource variable which does not change
  // a value, then add a clone/copy to make sure it does not get overwritten
  // between runs
  if (in_info.IsResourceNotModified()) {
    poplar::Tensor non_modified_tensor = tensor;
    poplar::Graph& graph =
        GetGraphWithOutputIndex(resources_, inst, flat_tuple_index);
    tensor = graph.clone(non_modified_tensor,
                         {debug_name_and_id, "resource_not_modified_clone"});

    // Call the base class since we do not want our own handling of
    // parameters for this special case.
    TF_RETURN_IF_ERROR(DeferredVisitor::AddSequenceForInstruction(
        inst, poplar::program::Copy(non_modified_tensor, tensor, false,
                                    {debug_name_and_id})));
  }
  return tensor;
}

const poplar::program::Sequence
EntryVisitor::GetSequenceAndInitializeCounters() {
  poplar::program::Sequence seq({}, "InitializeCounters");
  seq.add(execution_counters_.SetInitialValuesToZero());
  seq.add(DeferredVisitor::GetSequence(/*copy_execution_counters*/ false));
  return seq;
}

Status EntryVisitor::FinishDeferedAllocationVisit(HloInstruction* root) {
  VLOG(1) << "Processing FinishVisit";
  poplar::DebugNameAndId debug_name_and_id = GetDebugNameAndId(root);

  HloComputation* comp = root->parent();

  if (ShapeUtil::IsEmptyTuple(root->shape())) {
    VLOG(1) << "Root instruction shape is an empty tuple";
    return Status::OK();
  }
  poplar::Graph& graph = GetGraph(resources_, root);

  auto* layout = comp->parent()->mutable_entry_computation_layout();
  const Shape layout_shape = layout->result_shape();

  const auto& entry_outputs =
      resources_.annotations.input_output_aliasing_map.GetEntryOutputInfos();

  const uint64 num_outputs =
      root->shape().IsTuple() ? ShapeUtil::TupleElementCount(root->shape()) : 1;

  CHECK_EQ(num_outputs, entry_outputs.size());

  for (uint64 idx = 0, output_tuple_index = 0; idx != entry_outputs.size();
       ++idx) {
    auto& out_info = entry_outputs[idx];

    poplar::program::Sequence seq({}, debug_name_and_id);

    // Flatten the tuple tensor (if required) and iterate over all of them
    const Shape layout_sub_shape =
        layout_shape.IsTuple()
            ? ShapeUtil::GetTupleElementShape(layout_shape, idx)
            : layout_shape;

    const std::vector<Shape> layout_sub_shapes =
        FlattenedXlaShape(layout_sub_shape);

    const uint64 flat_tuple_index_start = output_tuple_index;
    const uint64 flat_tuple_index_end =
        flat_tuple_index_start + layout_sub_shapes.size();
    output_tuple_index = flat_tuple_index_end;

    TF_RETURN_IF_ERROR(CheckNoOpaqueTypes(root));

    // Check whether this is a dummy of inserted by a remote buffer - if it is
    // then we do not add copies/FIFO for it.
    if (out_info.IsResourceModified()) {
      const int64 param_number = out_info.GetInputIndex();
      if (IsRemoteParameter(param_number, resources_)) {
        if (resources_.use_verified_transfers) {
          return xla::FailedPrecondition(
              "Parameter offloading cannot be used at the same time as "
              "verified streams");
        }
        continue;
      }
    }

    // Get the all the tensors for the current output index - work out the
    // range.
    TF_ASSIGN_OR_RETURN(TensorVector out_tensors,
                        FindExpandedInstructionOutputsInRange(
                            tensor_map, resources_, root,
                            {flat_tuple_index_start, flat_tuple_index_end}, seq,
                            debug_name_and_id));

    // If the output is a modified resource, we want to keep it on the device at
    // the exact same location it was an input to the graph.
    if (out_info.IsResourceModified()) {
      // Get the inputs to the graph, and if the input and output tensors do not
      // match, add a on device copy to make sure location of the resource
      // variable doesn't change between the runs (the alternative is to reload
      // the graph everytime).
      TF_ASSIGN_OR_RETURN(
          TensorVector in_tensors,
          FindInstructionOutputTensorsInRange(
              tensor_map, resources_,
              comp->parameter_instruction(out_info.GetInputIndex()),
              {0, layout_sub_shapes.size()}));
      for (uint64 tuple_index = 0; tuple_index != layout_sub_shapes.size();
           ++tuple_index) {
        if (in_tensors[tuple_index] != out_tensors[tuple_index]) {
          AddSequenceForInstruction(
              root, poplar::program::Copy(out_tensors[tuple_index],
                                          in_tensors[tuple_index], false,
                                          debug_name_and_id));
        }
      }
    }

    if (!UseSyntheticDataFor(SyntheticDataCategory::Parameters)) {
      // Add FIFOs to the host for each output tensor.
      for (uint64 tuple_index = 0; tuple_index != layout_sub_shapes.size();
           ++tuple_index) {
        poplar::Tensor out = ConvertFromDeviceLayout(
            layout_sub_shapes[tuple_index], out_tensors[tuple_index]);

        const std::string handle = out_info.Handles().at(tuple_index);
        auto fifo = graph.addDeviceToHostFIFO(
            handle, out.elementType(), out.numElements(),
            resources_.streams_indices.GraphOptions(handle));

        TF_RETURN_IF_ERROR(AddDeviceToHostCopy(
            out, fifo,
            !out_info.IsStreaming() ||
                resources_.always_rearrange_copies_on_host,
            graph, resources_, seq, out_info, root, debug_name_and_id));

        OutputInfo info = {out_info.Name(), handle, tuple_index,
                           layout_sub_shapes[tuple_index]};
        TF_RETURN_IF_ERROR(AddEntryOutputInfo(resources_.annotations, info));
      }
    }

    if (out_info.IsStreaming()) {
      AddSequenceForInstruction(root, seq);
    } else {
      device_to_host.add(seq);
    }
  }

  return Status::OK();
}

const poplar::program::Sequence EntryVisitor::GetHostToDevice() const {
  poplar::program::Sequence combined({}, {"HostToDevice"});
  combined.add(resources_.streams_indices.LoadCheckpointSequence());
  combined.add(host_to_device);
  return combined;
}

const poplar::program::Sequence EntryVisitor::GetDeviceToHost() const {
  poplar::program::Sequence combined({}, {"DeviceToHost"});
  combined.add(device_to_host);
  combined.add(resources_.streams_indices.SaveCheckpointSequence());
  return combined;
}

}  // namespace poplarplugin
}  // namespace xla
