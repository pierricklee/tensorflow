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

#ifndef TENSORFLOW_COMPILER_PLUGIN_POPLAR_DRIVER_TOOLS_CUSTOM_OPS_CODELET_EXPRESSION_OP_H_
#define TENSORFLOW_COMPILER_PLUGIN_POPLAR_DRIVER_TOOLS_CUSTOM_OPS_CODELET_EXPRESSION_OP_H_

#include "tensorflow/compiler/plugin/poplar/driver/tools/custom_ops/hlo_poplar_instruction.h"

namespace xla {
namespace poplarplugin {

class HloCodeletExpressionOpInstruction : public HloPoplarInstruction {
 public:
  explicit HloCodeletExpressionOpInstruction(
      const xla::Shape& shape, absl::Span<HloInstruction* const> operands,
      const std::string& source);

  absl::flat_hash_set<int64> AllocatingIndices() const override;
  bool AllocatingOutput() const override;
  absl::flat_hash_map<int64, int64> LayoutDependencies() const override;
  HloPoplarUseDescriptions GetUseDescriptions() const override;
  HloPoplarBufferDescriptions GetBufferDescriptions() const override;

  const FindConsumersExtensionResults FindConsumers(
      FindConsumersExtensionParams params) const override;

  bool IsPopOpsElementwise() const override;

  const std::string& source() const;

 protected:
  virtual std::vector<string> ExtraPoplarAttributesToStringImpl(
      const HloPrintOptions& options) const;

 private:
  const std::string source_;

  std::unique_ptr<HloInstruction> CloneWithNewOperandsImpl(
      const Shape& shape, absl::Span<HloInstruction* const>,
      HloCloneContext*) const override;
};

}  // namespace poplarplugin
}  // namespace xla

#endif
