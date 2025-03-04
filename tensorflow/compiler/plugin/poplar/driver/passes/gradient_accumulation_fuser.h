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

#ifndef TENSORFLOW_COMPILER_PLUGIN_POPLAR_DRIVER_PASSES_GRADIENT_ACCUMULATION_FUSER_H_
#define TENSORFLOW_COMPILER_PLUGIN_POPLAR_DRIVER_PASSES_GRADIENT_ACCUMULATION_FUSER_H_

#include "tensorflow/compiler/plugin/poplar/driver/tools/hlo_matcher.h"
#include "tensorflow/core/framework/types.h"

namespace xla {

namespace poplarplugin {

// The purpose of this pass is to fuse any operations specific to gradient
// accumulation.
class GradientAccumulationFuser : public HloMatcher {
 public:
  GradientAccumulationFuser(struct CompilerAnnotations& annotations);

  ~GradientAccumulationFuser() override = default;

  absl::string_view name() const override {
    return "gradient-accumulation-fuser";
  }

 private:
  StatusOr<bool> HandleMatch(HloMatcherMatched& match,
                             const absl::optional<int64>) override;
};

}  // namespace poplarplugin
}  // namespace xla

#endif
