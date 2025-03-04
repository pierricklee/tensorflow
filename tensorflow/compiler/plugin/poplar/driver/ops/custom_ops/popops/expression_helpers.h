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
#ifndef TENSORFLOW_COMPILER_PLUGIN_POPLAR_DRIVER_OPS_CUSTOM_OPS_POPOPS_EXPRESSION_HELPERS_H_
#define TENSORFLOW_COMPILER_PLUGIN_POPLAR_DRIVER_OPS_CUSTOM_OPS_POPOPS_EXPRESSION_HELPERS_H_
#include <algorithm>
#include <memory>
#include <poplar/Engine.hpp>
#include <poplar/Graph.hpp>
#include <utility>
#include <vector>

#include "tensorflow/compiler/plugin/poplar/driver/backend_config.pb.h"
#include "tensorflow/compiler/plugin/poplar/driver/compiler_resources.h"
#include "tensorflow/compiler/plugin/poplar/driver/ops/ops.h"
#include "tensorflow/compiler/plugin/poplar/driver/tensor.h"
#include "tensorflow/compiler/plugin/poplar/driver/tools/matmul_util.h"
#include "tensorflow/compiler/plugin/poplar/driver/tools/ml_type_helper.h"
#include "tensorflow/compiler/plugin/poplar/driver/tools/util.h"
#include "tensorflow/compiler/xla/primitive_util.h"
#include "tensorflow/compiler/xla/shape_util.h"
#include "tensorflow/compiler/xla/status_macros.h"

namespace xla {
namespace poplarplugin {
namespace helper {

// Helper struct for generating the popops expression for elementwise operation.
struct ExpressionInput {
  std::unique_ptr<popops::expr::Expr> expr;
  absl::optional<poplar::Tensor> tensor;
  ExpressionInput() = delete;

  explicit ExpressionInput(poplar::Tensor& tensor) : tensor(tensor) {}
  explicit ExpressionInput(std::unique_ptr<popops::expr::Expr> expr)
      : expr(std::move(expr)), tensor(absl::nullopt) {}

  ExpressionInput(const ExpressionInput& other) {
    if (other.expr) {
      expr = other.expr->clone();
    }
    tensor = other.tensor;
  }
};
using ExpressionInputs = std::vector<ExpressionInput>;

std::vector<poplar::Tensor> GetTensorsFromExpressionInputs(
    ExpressionInputs& expression_inputs);

// Get the elementwise instruction when the instruction can be a fused
// instruction indicating implicit broadcasting op.
const HloInstruction* GetElementwiseOp(const HloInstruction* inst);

// Get all the elementwise input expression and tensors.
StatusOr<ExpressionInputs> GetElementwiseInputs(
    CompilerResources& res, const HloInstruction* inst,
    const std::vector<int64>& inputs_permutation, TensorMap& tensor_map,
    poplar::program::Sequence& seq,
    const poplar::DebugNameAndId& debug_name_and_id);

}  // namespace helper
}  // namespace poplarplugin
}  // namespace xla

#endif  // TENSORFLOW_COMPILER_PLUGIN_POPLAR_DRIVER_OPS_CUSTOM_OPS_POPOPS_EXPRESSION_HELPERS_H_
