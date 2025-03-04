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

#ifndef TENSORFLOW_COMPILER_PLUGIN_POPLAR_DRIVER_POPLAR_ALGEBRAIC_SIMPLIFIER_H_
#define TENSORFLOW_COMPILER_PLUGIN_POPLAR_DRIVER_POPLAR_ALGEBRAIC_SIMPLIFIER_H_

#include <utility>

#include "tensorflow/compiler/xla/service/hlo_module.h"
#include "tensorflow/compiler/xla/service/hlo_pass_interface.h"

namespace xla {

// A pass which performs algebraic simplifications.
class PoplarAlgebraicSimplifier : public HloModulePass {
 public:
  explicit PoplarAlgebraicSimplifier(bool enable_fast_math = false);
  ~PoplarAlgebraicSimplifier() override = default;
  absl::string_view name() const override {
    return "poplar-algebraic-simplifier";
  }

  // Run algebraic simplification on the given computation. Returns whether the
  // computation was changed.
  StatusOr<bool> Run(HloModule* module) override;

  // Create constant from literal with tiles and element size updated in the
  // constant's layout.
  std::unique_ptr<HloInstruction> CreateConstantWithLayoutUpdated(
      Literal literal) {
    auto constant = HloInstruction::CreateConstant(std::move(literal));
    UpdateLayout(constant->mutable_shape());
    return constant;
  }

 private:
  const bool enable_fast_math_;
};

}  // namespace xla

#endif  // TENSORFLOW_COMPILER_PLUGIN_POPLAR_DRIVER_POPLAR_ALGEBRAIC_SIMPLIFIER_H_
