/* Copyright 2017 The TensorFlow Authors. All Rights Reserved.

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

#ifndef TENSORFLOW_COMPILER_PLUGIN_POPLAR_DRIVER_OPS_OPS_HELPER_H_
#define TENSORFLOW_COMPILER_PLUGIN_POPLAR_DRIVER_OPS_OPS_HELPER_H_

#include <string>
#include <vector>

#include <poplar/exceptions.hpp>
#include <poputil/exceptions.hpp>

#include "tensorflow/compiler/plugin/poplar/driver/ops/ops.h"
#include "tensorflow/compiler/xla/service/hlo_opcode.h"
#include "tensorflow/compiler/xla/statusor.h"
#include "tensorflow/compiler/xla/xla_data.pb.h"
#include "tensorflow/core/lib/core/status.h"
#include "tensorflow/core/lib/gtl/array_slice.h"

namespace poplar {
class Graph;
class Tensor;
}  // namespace poplar

namespace xla {
class HloInstruction;

namespace poplarplugin {

struct CompilerResources;
struct TensorTarget;

StatusOr<poplar::Tensor> AllocatePoplarOpTensor(
    poplar::Graph& graph, CompilerResources& res,
    const poplar::DebugNameAndId& debug_name_and_id,
    const TensorTarget& tensor_target, const xla::Shape& shape,
    const TensorMap& tensor_map);

StatusOr<poplar::program::Program> CreatePoplarOp(
    poplar::Graph& graph, CompilerResources& res, const HloInstruction* inst,
    const xla::Shape& output, TensorMap& tensor_map,
    const poplar::DebugNameAndId& debug_name_and_id);

StatusOr<poplar::Tensor> AllocateHloOpTensor(
    poplar::Graph& graph, CompilerResources& res,
    const poplar::DebugNameAndId& debug_name_and_id,
    const TensorTarget& tensor_target, const xla::Shape& shape,
    const TensorMap& tensor_map);

StatusOr<poplar::program::Program> CreateHloOp(poplar::Graph& graph,
                                               CompilerResources& res,
                                               const HloInstruction* inst,
                                               const xla::Shape& output,
                                               TensorMap& tensor_map);
}  // namespace poplarplugin
}  // namespace xla

#endif  // TENSORFLOW_COMPILER_PLUGIN_POPLAR_DRIVER_OPS_OPS_HELPER_H_
