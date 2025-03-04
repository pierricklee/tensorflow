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

#ifndef TENSORFLOW_COMPILER_PLUGIN_POPLAR_DRIVER_PASSES_REMOTE_PARAMETER_PARALLEL_COMBINER_H_
#define TENSORFLOW_COMPILER_PLUGIN_POPLAR_DRIVER_PASSES_REMOTE_PARAMETER_PARALLEL_COMBINER_H_

#include "tensorflow/compiler/plugin/poplar/driver/passes/allocation_finder.h"
#include "tensorflow/compiler/xla/service/hlo_pass_interface.h"

namespace xla {
class HloInstruction;
class HloModule;

namespace poplarplugin {

/**
 * This pass tries to combine remote parameter loads and stores that can be
 * executed in parallel.
 */
class RemoteParameterParallelCombiner : public HloModulePass {
 public:
  absl::string_view name() const override {
    return "remote-parameter-parallel-combiner";
  }

  StatusOr<bool> Run(HloModule* module) override;

  StatusOr<bool> RunOnComputation(HloComputation* comp);
};

}  // namespace poplarplugin
}  // namespace xla

#endif  // TENSORFLOW_COMPILER_PLUGIN_POPLAR_DRIVER_PASSES_REMOTE_PARAMETER_PARALLEL_COMBINER_H_
