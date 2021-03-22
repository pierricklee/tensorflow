/* Copyright 2019 The TensorFlow Authors. All Rights Reserved.

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

#include "tensorflow/compiler/plugin/poplar/driver/tools/custom_ops/all_gather.h"

#include <string>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "tensorflow/compiler/plugin/poplar/driver/tools/hlo_poplar_buffer_util.h"
#include "tensorflow/compiler/plugin/poplar/kernels/custom_kernels_util.h"
#include "tensorflow/compiler/plugin/poplar/kernels/ops.pb.h"

namespace xla {
namespace poplarplugin {

// Constructor.
HloPoplarAllGatherInstruction::HloPoplarAllGatherInstruction(
    std::vector<HloInstruction*> inputs, const Shape& output_shape)
    : HloPoplarInstruction(output_shape, inputs, PoplarOp::AllGather) {}

absl::flat_hash_set<int64> HloPoplarAllGatherInstruction::AllocatingIndices()
    const {
  return {};
}

bool HloPoplarAllGatherInstruction::AllocatingOutput() const { return false; }

absl::flat_hash_map<int64, int64>
HloPoplarAllGatherInstruction::LayoutDependencies() const {
  return {};
}

HloPoplarUseDescriptions HloPoplarAllGatherInstruction::GetUseDescriptions()
    const {
  return UseDescriptionsNoInputOutputAlias();
}

HloPoplarBufferDescriptions
HloPoplarAllGatherInstruction::GetBufferDescriptions() const {
  return BufferDescriptionsAllocatesAllOutputs(this);
}

bool HloPoplarAllGatherInstruction::IsPopOpsElementwise() const {
  return false;
}

// Creates an instance of a HloPoplarAllGatherInstruction
std::unique_ptr<HloInstruction> CreatePoplarAllGather(
    std::vector<HloInstruction*> inputs, const Shape& output_shape) {
  return absl::make_unique<HloPoplarAllGatherInstruction>(inputs, output_shape);
}

std::unique_ptr<HloInstruction>
HloPoplarAllGatherInstruction::CloneWithNewOperandsImpl(
    const Shape& shape, absl::Span<HloInstruction* const> operands,
    HloCloneContext*) const {
  return CreatePoplarAllGather({operands.begin(), operands.end()}, shape);
}

std::vector<std::string>
HloPoplarAllGatherInstruction::ExtraPoplarAttributesToStringImpl(
    const HloPrintOptions& options) const {
  return {};
}

namespace {

static HloPoplarInstructionFactory allgather_factory(
    PoplarOp::AllGather,
    [](HloCustomCallInstruction* call)
        -> StatusOr<std::unique_ptr<HloInstruction>> {
      CHECK_EQ(call->operand_count(), 1);
      return CreatePoplarAllGather({call->mutable_operand(0)}, call->shape());
    });

}  // namespace

}  // namespace poplarplugin
}  // namespace xla
