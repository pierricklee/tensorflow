/* Copyright 2020 The TensorFlow Authors. All Rights Reserved.

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

#ifndef TENSORFLOW_COMPILER_PLUGIN_POPLAR_DRIVER_TOOLS_CUSTOM_OPS_REDUCE_SCATTER_H_
#define TENSORFLOW_COMPILER_PLUGIN_POPLAR_DRIVER_TOOLS_CUSTOM_OPS_REDUCE_SCATTER_H_

#include <memory>
#include <string>
#include <vector>

#include "tensorflow/compiler/plugin/poplar/driver/tools/custom_ops/hlo_poplar_instruction.h"
#include "tensorflow/compiler/plugin/poplar/driver/tools/poplar_replica_groups.h"

namespace xla {
namespace poplarplugin {

class HloReduceScatterInstruction : public HloPoplarInstruction {
 public:
  explicit HloReduceScatterInstruction(const Shape outputShape,
                                       absl::Span<HloInstruction* const> inputs,
                                       CollectiveOperator op,
                                       PoplarReplicaGroups replica_groups);

  absl::flat_hash_set<int64> AllocatingIndices() const override;
  bool AllocatingOutput() const override;

  absl::flat_hash_map<int64, int64> LayoutDependencies() const override;

  HloPoplarUseDescriptions GetUseDescriptions() const override;
  HloPoplarBufferDescriptions GetBufferDescriptions() const override;

  const FindConsumersExtensionResults FindConsumers(
      FindConsumersExtensionParams params) const override;

  bool IsPopOpsElementwise() const override;

  CollectiveOperator GetCollectiveOperator() const;
  PoplarReplicaGroups GetPoplarReplicaGroups() const;

 protected:
  std::vector<std::string> ExtraPoplarAttributesToStringImpl(
      const HloPrintOptions& options) const override;

 private:
  CollectiveOperator op_;

  std::unique_ptr<HloInstruction> CloneWithNewOperandsImpl(
      const Shape& shape, absl::Span<HloInstruction* const>,
      HloCloneContext*) const override;

  PoplarReplicaGroups replica_groups_;
};

std::unique_ptr<HloInstruction> CreateReduceScatter(
    const Shape& shape, absl::Span<HloInstruction* const> input,
    CollectiveOperator op, PoplarReplicaGroups replica_groups = {});

}  // namespace poplarplugin
}  // namespace xla

#endif  // TENSORFLOW_COMPILER_PLUGIN_POPLAR_DRIVER_TOOLS_CUSTOM_OPS_REDUCE_SCATTER_H_
