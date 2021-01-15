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

#include <algorithm>
#include <poplar/Graph.hpp>
#include <poplar/Tensor.hpp>
#include <popops/ElementWise.hpp>
#include <unordered_map>
#include <unordered_set>

extern "C" {
int32_t custom_op_api_level = 2;
}

namespace pe = popops::expr;

// Custom poplar kernel.
extern "C" poplar::program::Program Build(
    poplar::Graph& graph, const std::vector<poplar::Tensor>& inputs,
    std::vector<poplar::Tensor>& outputs, const std::string& attributes,
    const std::string& debugPrefix) {
  poplar::program::Sequence seq;
  return seq;
}

extern "C" void Build_metadata(std::vector<std::int64_t>& allocating_indices,
                               std::uint32_t& num_inplace, bool& is_elementwise,
                               bool& is_stateless, std::uint32_t num_inputs) {
  num_inplace = 12;
  is_elementwise = true;

  allocating_indices.insert(0);
  allocating_indices.insert(1);
  allocating_indices.insert(2);
  allocating_indices.insert(3);

  layout_dependencies[0] = 2;
  layout_dependencies[1] = 3;
}
