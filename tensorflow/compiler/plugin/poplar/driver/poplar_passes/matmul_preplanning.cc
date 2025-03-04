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
#include "tensorflow/compiler/plugin/poplar/driver/poplar_passes/matmul_preplanning.h"

#include <utility>
#include <vector>

#include "tensorflow/compiler/plugin/poplar/driver/tensor.h"
#include "tensorflow/compiler/plugin/poplar/driver/tools/custom_ops/lstm.h"
#include "tensorflow/compiler/plugin/poplar/driver/tools/matcher_predicates.h"
#include "tensorflow/compiler/plugin/poplar/driver/tools/matmul_util.h"
#include "tensorflow/compiler/plugin/poplar/driver/tools/poplar_util.h"
#include "tensorflow/compiler/plugin/poplar/driver/tools/rnn_util.h"

#include "tensorflow/compiler/xla/service/hlo_casting_utils.h"

#include <poplin/Cholesky.hpp>
#include <poplin/TriangularSolve.hpp>
#include <popnn/Recurrent.hpp>

namespace xla {
namespace poplarplugin {
namespace {

StatusOr<std::vector<size_t>> DimShuffleShape(
    const std::vector<size_t>& shape,
    const std::vector<unsigned>& permutation) {
  if (permutation.size() != shape.size()) {
    return xla::FailedPrecondition(
        "DimShuffleShape permutation size %d does not match shape size %d.",
        permutation.size(), shape.size());
  }

  std::vector<size_t> shuffled_shape;
  for (size_t i = 0; i != permutation.size(); ++i) {
    shuffled_shape.push_back(shape.at(permutation.at(i)));
  }

  return shuffled_shape;
}

StatusOr<poplin::MatMulParams> GetMatMulParams(const HloInstruction* inst,
                                               CompilerResources& res) {
  const DotDimensionNumbers dot_dims = inst->dot_dimension_numbers();

  const Shape& lhs_shape = inst->operand(0)->shape();
  const Shape& rhs_shape = inst->operand(1)->shape();
  std::vector<size_t> lhs_shape_inst(lhs_shape.dimensions().begin(),
                                     lhs_shape.dimensions().end());
  std::vector<size_t> rhs_shape_inst(rhs_shape.dimensions().begin(),
                                     rhs_shape.dimensions().end());

  // DimShuffle the LHS to [Batch..., M..., Contracting...]
  std::vector<unsigned> lhs_permutation =
      LeftMatMulPermutations(lhs_shape_inst, dot_dims);
  TF_ASSIGN_OR_RETURN(std::vector<size_t> lhs_shape_shuffled,
                      DimShuffleShape(lhs_shape_inst, lhs_permutation));

  // DimShuffle the RHS to [Batch..., Contracting..., N...]
  std::vector<unsigned> rhs_permutation =
      RightMatMulPermutations(rhs_shape_inst, dot_dims);
  TF_ASSIGN_OR_RETURN(std::vector<size_t> rhs_shape_shuffled,
                      DimShuffleShape(rhs_shape_inst, rhs_permutation));

  // Collapse the LHS dimensions down to [Batch, M, Contracting]
  std::vector<size_t> lhs_shape_packed =
      LeftMatMulPackShape(lhs_shape_shuffled, dot_dims);

  // Collapse the RHS dimensions down to [Batch, Contracting, N]
  std::vector<size_t> rhs_shape_packed =
      RightMatMulPackShape(rhs_shape_shuffled, dot_dims);

  TF_ASSIGN_OR_RETURN(poplar::Type input_type, PoplarDataType(lhs_shape));
  TF_ASSIGN_OR_RETURN(poplar::Type output_type, PoplarDataType(inst->shape()));

  poplin::MatMulParams matmul_params;
  matmul_params.inputType = input_type;
  matmul_params.outputType = output_type;
  matmul_params.aShape = lhs_shape_packed;
  matmul_params.bShape = rhs_shape_packed;

  return matmul_params;
}

Status GetGruOptsForMatMulPreplanning(const HloInstruction* inst,
                                      const CompilerResources& res,
                                      poplar::Type& partials_poplar_type,
                                      bool& inference_only) {
  auto gru_inst = Cast<HloRNNInstruction>(inst);

  inference_only = !gru_inst->is_training();

  // Get the partial type.
  xla::PrimitiveType partials_xla_type = gru_inst->partials_type();

  TF_ASSIGN_OR_RETURN(poplar::Type partials_poplar_type_tmp,
                      PoplarDataType(partials_xla_type));

  partials_poplar_type = partials_poplar_type_tmp;

  return Status::OK();
}

}  // namespace

Status MatMulPreplanning::StorePreplanMatMulsLSTM(const HloInstruction* inst) {
  const poplar::Target& target = GetGraph(resources_, inst).getTarget();

  TF_ASSIGN_OR_RETURN(popnn::lstm::LstmParams lstm_params,
                      GetLstmParameters(inst));
  TF_ASSIGN_OR_RETURN(poplar::OptionFlags option_flags,
                      GetLstmOpts(inst, resources_));

  const std::vector<std::pair<poplin::MatMulParams, poplar::OptionFlags>>
      mat_muls_to_pre_plan =
          popnn::lstm::getMatMulPrePlanParameters(lstm_params, option_flags);

  for (const auto& mat_mul : mat_muls_to_pre_plan) {
    option_flags_store.push_back(mat_mul.second);
    preplan_matmuls.emplace(&target, mat_mul.first,
                            &(option_flags_store.back()));
  }

  return Status::OK();
}

Status MatMulPreplanning::StorePreplanMatMulsGRU(const HloInstruction* inst) {
  const poplar::Target& target = GetGraph(resources_, inst).getTarget();

  TF_ASSIGN_OR_RETURN(popnn::gru::GruParams gru_params, GetGruParameters(inst));

  bool inference_only;
  poplar::Type partials_poplar_type;
  GetGruOptsForMatMulPreplanning(inst, resources_, partials_poplar_type,
                                 inference_only);

  const std::vector<std::pair<poplin::MatMulParams, poplar::OptionFlags>>
      mat_muls_to_pre_plan = popnn::rnn::getMatMulPrePlanParameters(
          gru_params.rnn.timeSteps, gru_params.rnn.batchSize,
          gru_params.rnn.layerSizes[0], gru_params.rnn.layerSizes[1],
          gru_params.rnn.dataType, partials_poplar_type, inference_only, true);

  for (const auto& mat_mul : mat_muls_to_pre_plan) {
    option_flags_store.push_back(mat_mul.second);
    preplan_matmuls.emplace(&target, mat_mul.first,
                            &(option_flags_store.back()));
  }

  return Status::OK();
}

Status MatMulPreplanning::StorePreplanMatMulsCholesky(
    const HloInstruction* inst) {
  const poplar::Target& target = GetGraph(resources_, inst).getTarget();
  const HloCholeskyInstruction* as_solve = Cast<HloCholeskyInstruction>(inst);

  const Shape& shape = inst->operand(0)->shape();
  TF_ASSIGN_OR_RETURN(auto type, PoplarDataType(shape));

  auto poplar_shape = PoplarShapeFromXlaShape(shape);
  auto& options = as_solve->cholesky_options();

  TF_ASSIGN_OR_RETURN(poplar::OptionFlags poplar_options,
                      GetCholeskyOptionsForInst(inst, resources_));

  auto preplan_params = poplin::getCholeskyMatMulPrePlanParameters(
      type, poplar_shape, options.lower(), poplar_options);

  for (const auto& preplan_param : preplan_params) {
    option_flags_store.push_back(preplan_param.second);
    preplan_matmuls.emplace(&target, preplan_param.first,
                            &(option_flags_store.back()));
  }

  return Status::OK();
}

Status MatMulPreplanning::StorePreplanMatMulsTriangularSolve(
    const HloInstruction* inst) {
  const poplar::Target& target = GetGraph(resources_, inst).getTarget();
  const HloTriangularSolveInstruction* as_solve =
      Cast<HloTriangularSolveInstruction>(inst);

  const Shape& aShape = inst->operand(0)->shape();
  const Shape& bShape = inst->operand(1)->shape();
  TF_ASSIGN_OR_RETURN(auto aType, PoplarDataType(aShape));
  TF_ASSIGN_OR_RETURN(auto bType, PoplarDataType(bShape));

  auto aPoplarShape = PoplarShapeFromXlaShape(aShape);
  auto bPoplarShape = PoplarShapeFromXlaShape(bShape);

  auto& options = as_solve->triangular_solve_options();

  TF_ASSIGN_OR_RETURN(poplar::OptionFlags poplar_options,
                      GetTriangularSolveOptionsForInst(inst, resources_));

  const std::vector<std::pair<poplin::MatMulParams, poplar::OptionFlags>>
      mat_muls_to_pre_plan = poplin::getTriangularSolveMatMulPrePlanParameters(
          aType, bType, aPoplarShape, bPoplarShape, options.left_side(),
          options.lower(), poplar_options);

  VLOG(2) << "Preplanned " << mat_muls_to_pre_plan.size() << " mat muls for "
          << inst->ToString();

  for (const auto& mat_mul : mat_muls_to_pre_plan) {
    option_flags_store.push_back(mat_mul.second);
    preplan_matmuls.emplace(&target, mat_mul.first,
                            &(option_flags_store.back()));
  }

  return Status::OK();
}

Status MatMulPreplanning::StorePreplanMatMuls(const HloInstruction* inst) {
  const poplar::Target& target = GetGraph(resources_, inst).getTarget();

  TF_ASSIGN_OR_RETURN(poplar::OptionFlags option_flags,
                      GetMatMulOptionsForInst(inst, resources_));

  TF_ASSIGN_OR_RETURN(poplin::MatMulParams mat_mul_params,
                      GetMatMulParams(inst, resources_));

  option_flags_store.push_back(option_flags);
  preplan_matmuls.emplace(&target, mat_mul_params,
                          &(option_flags_store.back()));

  return Status::OK();
}

/*
 * Visit all non-fused operations in the whole module looking for matmul,
 * and add the parameters and the options for that matmul to the set
 * of things to pass to the poplibs matmul pre-planner.
 */
StatusOr<bool> MatMulPreplanning::Run(HloModule* module) {
  preplan_matmuls.clear();
  option_flags_store.clear();

  for (auto* comp : module->computations()) {
    if (!IsPopOpsFusion(comp)) {
      for (HloInstruction* inst : comp->instructions()) {
        if (inst->opcode() == HloOpcode::kDot) {
          StorePreplanMatMuls(inst);
        } else if (inst->opcode() == HloOpcode::kCholesky) {
          StorePreplanMatMulsCholesky(inst);
        } else if (inst->opcode() == HloOpcode::kTriangularSolve) {
          StorePreplanMatMulsTriangularSolve(inst);
        } else if (IsPoplarInstruction(PoplarOp::LstmLayerFwd)(inst) ||
                   IsPoplarInstruction(PoplarOp::LstmLayerBwd)(inst)) {
          StorePreplanMatMulsLSTM(inst);
        } else if (IsPoplarInstruction(PoplarOp::GRULayerFwd)(inst) ||
                   IsPoplarInstruction(PoplarOp::GRULayerBwd)(inst)) {
          StorePreplanMatMulsGRU(inst);
        }
      }
    }
  }

  poplin::preplanMatMuls(preplan_matmuls, resources_.matmul_cache);
  return false;
}

}  // namespace poplarplugin
}  // namespace xla
