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

#ifndef TENSORFLOW_COMPILER_PLUGIN_POPLAR_DRIVER_POPLAR_PASSES_EMBEDDING_PLANS_PREPLANNING_H_
#define TENSORFLOW_COMPILER_PLUGIN_POPLAR_DRIVER_POPLAR_PASSES_EMBEDDING_PLANS_PREPLANNING_H_

#include "tensorflow/compiler/plugin/poplar/driver/compiler_resources.h"
#include "tensorflow/compiler/xla/service/hlo_pass_interface.h"

namespace xla {

class HloModule;

namespace poplarplugin {

/**
 * Find all the tensors that will require a SlicePlan at creation.
 */
class EmbeddingPlansPreplanning : public HloModulePass {
 public:
  explicit EmbeddingPlansPreplanning(CompilerResources& resources)
      : resources_(resources) {}

  absl::string_view name() const override {
    return "embedding-plans-preplanning";
  }

  StatusOr<bool> Run(HloModule* module) override;

 private:
  CompilerResources& resources_;
};
}  // namespace poplarplugin
}  // namespace xla

#endif  // TENSORFLOW_COMPILER_PLUGIN_POPLAR_DRIVER_POPLAR_PASSES_EMBEDDING_PLANS_PREPLANNING_H_
