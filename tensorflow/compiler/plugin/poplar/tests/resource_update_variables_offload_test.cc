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

#include "tensorflow/compiler/plugin/poplar/driver/passes/resource_update_variables_offload.h"

#include <algorithm>

#include "tensorflow/compiler/plugin/poplar/driver/compiler_annotations.h"
#include "tensorflow/compiler/plugin/poplar/driver/tools/custom_ops/remote_parameter.h"
#include "tensorflow/compiler/plugin/poplar/driver/tools/matcher_predicates.h"
#include "tensorflow/compiler/plugin/poplar/driver/tools/pipeline_util.h"
#include "tensorflow/compiler/xla/service/hlo_casting_utils.h"
#include "tensorflow/compiler/xla/service/pattern_matcher.h"
#include "tensorflow/compiler/xla/test.h"
#include "tensorflow/compiler/xla/tests/hlo_test_base.h"

namespace xla {
namespace m = match;
namespace poplarplugin {
namespace {

using ResourceUpdateVariablesOffloadTest = HloTestBase;
std::string GetHlo(ThreeState offload_variables) {
  constexpr absl::string_view hlo_format = R"(
HloModule top

stage_0_fwd {
  in0 = f32[1,4,4,2] parameter(0)
  in1 = f32[1,4,4,2] parameter(1)
  ROOT tuple = (f32[1,4,4,2], f32[1,4,4,2]) tuple(in0, in1)
}

stage_1_fwd {
  in1 = f32[1,4,4,2] parameter(0)
  in2 = f32[1,4,4,2] parameter(1)
  ROOT tuple = (f32[1,4,4,2], f32[1,4,4,2]) tuple(in1, in2)
}

stage_1_bwd {
  in1_grad = f32[1,4,4,2] parameter(0)
  in2_grad = f32[1,4,4,2] parameter(1)
  ROOT tuple = (f32[1,4,4,2], f32[1,4,4,2]) tuple(in1_grad, in2_grad)
}

stage_0_bwd {
  in0_grad = f32[1,4,4,2] parameter(0)
  in1_grad = f32[1,4,4,2] parameter(1)
  ROOT tuple = (f32[1,4,4,2], f32[1,4,4,2]) tuple(in0_grad, in1_grad)
}

resource_update {
  arg0 = f32[1,4,4,2] parameter(0)
  arg1 = f32[1,4,4,2] parameter(1)
  arg2 = f32[1,4,4,2] parameter(2)
  arg3 = f32[1,4,4,2] parameter(3)
  arg2_new = f32[1,4,4,2] add(arg2, arg0)
  arg3_new = f32[1,4,4,2] add(arg3, arg1)
  ROOT t = (f32[1,4,4,2], f32[1,4,4,2], f32[1,4,4,2]) tuple(arg0, arg1, arg2_new, arg3_new)
}

pipeline {
  after-all = token[] after-all()
  infeed = (f32[1,4,4,2], token[]) infeed(after-all), infeed_config="140121807314576"
  in0 = f32[1,4,4,2] get-tuple-element(infeed), index=0
  in1 = f32[1,4,4,2] parameter(0)
  in2 = f32[1,4,4,2] parameter(1)
  stage_0 = (f32[1,4,4,2], f32[1,4,4,2]) call(in1, in0), to_apply=stage_0_fwd, backend_config="{\"callConfig\":{\"type\":\"PipelineStage\",\"pipelineStageConfig\":{\"stageId\":\"0\"}}}", sharding={maximal device=0}
  stage_0_0 = f32[1,4,4,2] get-tuple-element(stage_0), index=0
  stage_0_1 = f32[1,4,4,2] get-tuple-element(stage_0), index=1
  stage_1 = (f32[1,4,4,2], f32[1,4,4,2]) call(stage_0_0, stage_0_1), to_apply=stage_1_fwd, backend_config="{\"callConfig\":{\"type\":\"PipelineStage\",\"pipelineStageConfig\":{\"stageId\":\"1\"}}}", sharding={maximal device=0}
  stage_1_1 = f32[1,4,4,2] get-tuple-element(stage_1), index=0
  stage_1_2 = f32[1,4,4,2] get-tuple-element(stage_1), index=1
  stage_1_bwd = (f32[1,4,4,2], f32[1,4,4,2]) call(stage_1_1, stage_1_2), to_apply=stage_1_bwd, backend_config="{\"callConfig\":{\"type\":\"PipelineStageBackward\",\"pipelineStageConfig\":{\"stageId\":\"1\"}}}", sharding={maximal device=0}
  stage_1_bwd_1 = f32[1,4,4,2] get-tuple-element(stage_1_bwd), index=0
  stage_0_bwd = (f32[1,4,4,2], f32[1,4,4,2]) call(stage_1_bwd_1, stage_0_0), to_apply=stage_0_bwd, backend_config="{\"callConfig\":{\"type\":\"PipelineStageBackward\",\"pipelineStageConfig\":{\"stageId\":\"0\"}}}", sharding={maximal device=0}
  stage_0_bwd_0 = f32[1,4,4,2] get-tuple-element(stage_0_bwd), index=0
  in3 = f32[1,4,4,2] parameter(2)
  in4 = f32[1,4,4,2] parameter(3)
  call_ru = (f32[1,4,4,2], f32[1,4,4,2], f32[1,4,4,2], f32[1,4,4,2]) call(stage_0_bwd_0, stage_1_bwd_1, in3, in4), to_apply=resource_update, frontend_attributes={CALL_CONFIG_TYPE=ResourceUpdate}, backend_config="{\"callConfig\":{\"type\":\"ResourceUpdate\",\"resourceUpdateConfig\":{\"offloadVariables\":\"%s\"}}}"
  gte0 = f32[1,4,4,2] get-tuple-element(call_ru), index=0
  gte1 = f32[1,4,4,2] get-tuple-element(call_ru), index=1
  gte2 = f32[1,4,4,2] get-tuple-element(call_ru), index=2
  gte3 = f32[1,4,4,2] get-tuple-element(call_ru), index=3
  ROOT tuple = (f32[1,4,4,2], f32[1,4,4,2], f32[1,4,4,2], f32[1,4,4,2]) tuple(gte0, gte1, gte2, gte3)
}

ENTRY e {
  e.in0 = f32[1,4,4,2] parameter(0), parameter_replication={false}
  e.in1 = f32[1,4,4,2] parameter(1), parameter_replication={false}
  e.in2 = f32[1,4,4,2] parameter(2), parameter_replication={false}
  e.in3 = f32[1,4,4,2] parameter(3), parameter_replication={false}
  e.call = (f32[1,4,4,2], f32[1,4,4,2], f32[1,4,4,2], f32[1,4,4,2]) call(e.in0, e.in1, e.in2, e.in3), to_apply=pipeline, backend_config="{\"callConfig\":{\"type\":\"Pipeline\"}}"
  gte0 = f32[1,4,4,2] get-tuple-element(e.call), index=0
  gte1 = f32[1,4,4,2] get-tuple-element(e.call), index=1
  gte2 = f32[1,4,4,2] get-tuple-element(e.call), index=2
  gte3 = f32[1,4,4,2] get-tuple-element(e.call), index=3
  ROOT t =  (f32[1,4,4,2], f32[1,4,4,2], f32[1,4,4,2], f32[1,4,4,2]) tuple(gte0, gte1, gte2, gte3)
}
)";
  return absl::StrFormat(hlo_format, ThreeState_Name(offload_variables));
}

TEST_F(ResourceUpdateVariablesOffloadTest, ReplaceRoot) {
  std::string hlo = R"(
HloModule top

stage_0_fwd {
  in0 = f32[1,4,4,2] parameter(0)
  in1 = f32[1,4,4,2] parameter(1)
  ROOT tuple = (f32[1,4,4,2], f32[1,4,4,2]) tuple(in0, in1)
}

stage_1_fwd {
  in1 = f32[1,4,4,2] parameter(0)
  in2 = f32[1,4,4,2] parameter(1)
  ROOT tuple = (f32[1,4,4,2], f32[1,4,4,2]) tuple(in1, in2)
}

stage_1_bwd {
  in1_grad = f32[1,4,4,2] parameter(0)
  in2_grad = f32[1,4,4,2] parameter(1)
  ROOT tuple = (f32[1,4,4,2], f32[1,4,4,2]) tuple(in1_grad, in2_grad)
}

stage_0_bwd {
  in0_grad = f32[1,4,4,2] parameter(0)
  in1_grad = f32[1,4,4,2] parameter(1)
  ROOT tuple = (f32[1,4,4,2], f32[1,4,4,2]) tuple(in0_grad, in1_grad)
}

resource_update {
  arg0 = f32[1,4,4,2] parameter(0)
  arg1 = f32[1,4,4,2] parameter(1)
  arg2 = f32[1,4,4,2] parameter(2)
  ROOT t = (f32[1,4,4,2], f32[1,4,4,2], f32[1,4,4,2]) tuple(arg0, arg1, arg2)
}

pipeline {
  after-all = token[] after-all()
  infeed = (f32[1,4,4,2], token[]) infeed(after-all), infeed_config="140121807314576"
  in0 = f32[1,4,4,2] get-tuple-element(infeed), index=0
  in1 = f32[1,4,4,2] parameter(0)
  in2 = f32[1,4,4,2] parameter(1)
  stage_0 = (f32[1,4,4,2], f32[1,4,4,2]) call(in1, in0), to_apply=stage_0_fwd, backend_config="{\"callConfig\":{\"type\":\"PipelineStage\",\"pipelineStageConfig\":{\"stageId\":\"0\"}}}", sharding={maximal device=0}
  stage_0_0 = f32[1,4,4,2] get-tuple-element(stage_0), index=0
  stage_0_1 = f32[1,4,4,2] get-tuple-element(stage_0), index=1
  stage_1 = (f32[1,4,4,2], f32[1,4,4,2]) call(stage_0_0, stage_0_1), to_apply=stage_1_fwd, backend_config="{\"callConfig\":{\"type\":\"PipelineStage\",\"pipelineStageConfig\":{\"stageId\":\"1\"}}}", sharding={maximal device=0}
  stage_1_1 = f32[1,4,4,2] get-tuple-element(stage_1), index=0
  stage_1_2 = f32[1,4,4,2] get-tuple-element(stage_1), index=1
  stage_1_bwd = (f32[1,4,4,2], f32[1,4,4,2]) call(stage_1_1, stage_1_2), to_apply=stage_1_bwd, backend_config="{\"callConfig\":{\"type\":\"PipelineStageBackward\",\"pipelineStageConfig\":{\"stageId\":\"1\"}}}", sharding={maximal device=0}
  stage_1_bwd_1 = f32[1,4,4,2] get-tuple-element(stage_1_bwd), index=0
  stage_1_bwd_2 = f32[1,4,4,2] get-tuple-element(stage_1_bwd), index=1
  stage_0_bwd = (f32[1,4,4,2], f32[1,4,4,2]) call(stage_1_bwd_1, stage_0_0), to_apply=stage_0_bwd, backend_config="{\"callConfig\":{\"type\":\"PipelineStageBackward\",\"pipelineStageConfig\":{\"stageId\":\"0\"}}}", sharding={maximal device=0}
  stage_0_bwd_0 = f32[1,4,4,2] get-tuple-element(stage_0_bwd), index=0
  call_ru = (f32[1,4,4,2], f32[1,4,4,2], f32[1,4,4,2]) call(stage_0_bwd_0, stage_1_bwd_1, stage_1_bwd_2), to_apply=resource_update, frontend_attributes={CALL_CONFIG_TYPE=ResourceUpdate}, backend_config="{\"callConfig\":{\"type\":\"ResourceUpdate\",\"resourceUpdateConfig\":{\"offloadVariables\":\"THREESTATE_ON\"}}}"
  gte0 = f32[1,4,4,2] get-tuple-element(call_ru), index=0
  gte1 = f32[1,4,4,2] get-tuple-element(call_ru), index=1
  gte2 = f32[1,4,4,2] get-tuple-element(call_ru), index=2
  ROOT tuple = (f32[1,4,4,2], f32[1,4,4,2], f32[1,4,4,2]) tuple(gte0, gte1, gte2)
}

ENTRY e {
  e.in0 = f32[1,4,4,2] parameter(0), parameter_replication={false}
  e.in1 = f32[1,4,4,2] parameter(1), parameter_replication={false}
  ROOT e.call = (f32[1,4,4,2], f32[1,4,4,2], f32[1,4,4,2]) call(e.in0, e.in1), to_apply=pipeline, backend_config="{\"callConfig\":{\"type\":\"Pipeline\"}}"
}
)";
  auto config = GetModuleConfigForTest();
  TF_ASSERT_OK_AND_ASSIGN(auto module,
                          ParseAndReturnVerifiedModule(hlo, config));
  auto root = module->entry_computation()->root_instruction();
  EXPECT_EQ(root->opcode(), HloOpcode::kCall);
  auto pipeline = root;

  CompilerAnnotations annotations(module.get());
  ResourceUpdateVariablesOffload prvo(annotations, true, 0, 1);
  TF_ASSERT_OK_AND_ASSIGN(bool changed, prvo.Run(module.get()));
  EXPECT_TRUE(changed);
  root = module->entry_computation()->root_instruction();
  EXPECT_EQ(root->opcode(), HloOpcode::kTuple);
  for (auto operand : root->operands()) {
    EXPECT_EQ(operand->opcode(), HloOpcode::kGetTupleElement);
    EXPECT_EQ(operand->operand(0), pipeline);
  }
}

TEST_F(ResourceUpdateVariablesOffloadTest, OffloadVariable) {
  std::string hlo = GetHlo(THREESTATE_ON);
  auto config = GetModuleConfigForTest();
  config.set_resource_input_count(4);
  config.set_input_mapping({0, 1, 2, 3});
  config.set_resource_update_to_input_index({0, 1, 2, 3});
  TF_ASSERT_OK_AND_ASSIGN(auto module,
                          ParseAndReturnVerifiedModule(hlo, config));

  CompilerAnnotations annotations(module.get());
  ResourceUpdateVariablesOffload prvo(annotations, true, 0, 1);
  TF_ASSERT_OK_AND_ASSIGN(bool changed, prvo.Run(module.get()));
  EXPECT_TRUE(changed);
  auto root = module->entry_computation()->root_instruction();
  HloComputation* pipeline_computation =
      root->operand(0)->operand(0)->to_apply();
  TF_ASSERT_OK_AND_ASSIGN(auto stages, GetPipelineStages(pipeline_computation));
  HloComputation* resource_update = (*stages.resource_update)->to_apply();
  EXPECT_EQ(resource_update->num_parameters(), 4);
  EXPECT_EQ(ShapeUtil::TupleElementCount(
                resource_update->root_instruction()->shape()),
            4);

  // Check load and stores.
  for (int64 i : {0, 1}) {
    HloInstruction* param = resource_update->parameter_instruction(i);
    EXPECT_EQ(param->user_count(), 2);
    HloInstruction* add = nullptr;
    for (auto* user : param->users()) {
      if (user->opcode() == HloOpcode::kAdd) {
        add = user;
        break;
      }
    }
    EXPECT_TRUE(add);
    const HloInstruction* load = add->operand(0);
    EXPECT_TRUE(IsPoplarInstruction(PoplarOp::RemoteParameterLoad)(load));
    EXPECT_EQ(add->user_count(), 1);
    const HloInstruction* store = add->users()[0];
    EXPECT_TRUE(IsPoplarInstruction(PoplarOp::RemoteParameterStore)(store));
  }
}

TEST_F(ResourceUpdateVariablesOffloadTest, OffloadVariableRepeat) {
  std::string hlo = R"(
HloModule top

resource_update {
  arg0 = f32[1,4,4,2] parameter(0)
  arg1 = f32[1,4,4,2] parameter(1)
  arg2 = f32[1,4,4,2] parameter(2)
  arg3 = f32[1,4,4,2] parameter(3)
  arg2_new = f32[1,4,4,2] add(arg2, arg0)
  arg3_new = f32[1,4,4,2] add(arg3, arg1)
  ROOT t = (f32[1,4,4,2], f32[1,4,4,2], f32[1,4,4,2]) tuple(arg0, arg1, arg2_new, arg3_new)
}

loop {
  after-all = token[] after-all()
  infeed = (f32[1,4,4,2], token[]) infeed(after-all), infeed_config="140121807314576"
  in0 = f32[1,4,4,2] get-tuple-element(infeed), index=0
  in1 = f32[1,4,4,2] parameter(0)
  add1 = f32[1,4,4,2] add(in0, in1)
  in2 = f32[1,4,4,2] parameter(1)
  add2 = f32[1,4,4,2] add(in0, in2)
  in3 = f32[1,4,4,2] parameter(2)
  in4 = f32[1,4,4,2] parameter(3)
  call_ru = (f32[1,4,4,2], f32[1,4,4,2], f32[1,4,4,2], f32[1,4,4,2]) call(add1, add2, in3, in4), to_apply=resource_update, frontend_attributes={CALL_CONFIG_TYPE=ResourceUpdate}, backend_config="{\"callConfig\":{\"type\":\"ResourceUpdate\",\"resourceUpdateConfig\":{\"offloadVariables\":\"THREESTATE_ON\"}}}"
  gte0 = f32[1,4,4,2] get-tuple-element(call_ru), index=0
  gte1 = f32[1,4,4,2] get-tuple-element(call_ru), index=1
  gte2 = f32[1,4,4,2] get-tuple-element(call_ru), index=2
  gte3 = f32[1,4,4,2] get-tuple-element(call_ru), index=3
  ROOT tuple = (f32[1,4,4,2], f32[1,4,4,2], f32[1,4,4,2], f32[1,4,4,2]) tuple(gte0, gte1, gte2, gte3)
}

ENTRY e {
  e.in0 = f32[1,4,4,2] parameter(0), parameter_replication={false}
  e.in1 = f32[1,4,4,2] parameter(1), parameter_replication={false}
  e.in2 = f32[1,4,4,2] parameter(2), parameter_replication={false}
  e.in3 = f32[1,4,4,2] parameter(3), parameter_replication={false}
  e.call = (f32[1,4,4,2], f32[1,4,4,2], f32[1,4,4,2], f32[1,4,4,2]) call(e.in0, e.in1, e.in2, e.in3), to_apply=loop, backend_config="{\"callConfig\":{\"type\":\"RepeatLoop\",\"repeatConfig\":{\"repeatCount\":\"100\"}}}"
  gte0 = f32[1,4,4,2] get-tuple-element(e.call), index=0
  gte1 = f32[1,4,4,2] get-tuple-element(e.call), index=1
  gte2 = f32[1,4,4,2] get-tuple-element(e.call), index=2
  gte3 = f32[1,4,4,2] get-tuple-element(e.call), index=3
  ROOT t =  (f32[1,4,4,2], f32[1,4,4,2], f32[1,4,4,2], f32[1,4,4,2]) tuple(gte0, gte1, gte2, gte3)
}
)";
  auto config = GetModuleConfigForTest();
  config.set_resource_input_count(4);
  config.set_input_mapping({0, 1, 2, 3});
  config.set_resource_update_to_input_index({0, 1, 2, 3});
  TF_ASSERT_OK_AND_ASSIGN(auto module,
                          ParseAndReturnVerifiedModule(hlo, config));

  CompilerAnnotations annotations(module.get());
  ResourceUpdateVariablesOffload prvo(annotations, true, 0, 1);
  TF_ASSERT_OK_AND_ASSIGN(bool changed, prvo.Run(module.get()));
  EXPECT_TRUE(changed);
  auto root = module->entry_computation()->root_instruction();
  HloComputation* repeat_computation = root->operand(0)->operand(0)->to_apply();
  HloInstruction* repeat_root = repeat_computation->root_instruction();
  HloComputation* resource_update =
      repeat_root->mutable_operand(0)->mutable_operand(0)->to_apply();
  EXPECT_EQ(resource_update->num_parameters(), 4);
  EXPECT_EQ(ShapeUtil::TupleElementCount(
                resource_update->root_instruction()->shape()),
            4);

  // Check load and stores.
  for (int64 i : {0, 1}) {
    HloInstruction* param = resource_update->parameter_instruction(i);
    EXPECT_EQ(param->user_count(), 2);
    HloInstruction* add = nullptr;
    for (auto* user : param->users()) {
      if (user->opcode() == HloOpcode::kAdd) {
        add = user;
        break;
      }
    }
    EXPECT_TRUE(add);
    const HloInstruction* load = add->operand(0);
    EXPECT_TRUE(IsPoplarInstruction(PoplarOp::RemoteParameterLoad)(load));
    EXPECT_EQ(add->user_count(), 1);
    const HloInstruction* store = add->users()[0];
    EXPECT_TRUE(IsPoplarInstruction(PoplarOp::RemoteParameterStore)(store));
  }
}

TEST_F(ResourceUpdateVariablesOffloadTest, Inference) {
  std::string hlo = R"(
HloModule top

stage_0_fwd {
  in0 = f32[1,4,4,2] parameter(0)
  in1 = f32[1,4,4,2] parameter(1)
  ROOT tuple = (f32[1,4,4,2], f32[1,4,4,2]) tuple(in0, in1)
}

stage_1_fwd {
  in1 = f32[1,4,4,2] parameter(0)
  in2 = f32[1,4,4,2] parameter(1)
  ROOT tuple = (f32[1,4,4,2], f32[1,4,4,2]) tuple(in1, in2)
}

pipeline {
  after-all = token[] after-all()
  infeed = (f32[1,4,4,2], token[]) infeed(after-all), infeed_config="140121807314576"
  in0 = f32[1,4,4,2] get-tuple-element(infeed), index=0
  in1 = f32[1,4,4,2] parameter(0)
  in2 = f32[1,4,4,2] parameter(1)
  stage_0 = (f32[1,4,4,2], f32[1,4,4,2]) call(in1, in0), to_apply=stage_0_fwd, backend_config="{\"callConfig\":{\"type\":\"PipelineStage\",\"pipelineStageConfig\":{\"stageId\":\"0\"}}}", sharding={maximal device=0}
  stage_0_0 = f32[1,4,4,2] get-tuple-element(stage_0), index=0
  stage_0_1 = f32[1,4,4,2] get-tuple-element(stage_0), index=1
  stage_1 = (f32[1,4,4,2], f32[1,4,4,2]) call(stage_0_0, stage_0_1), to_apply=stage_1_fwd, backend_config="{\"callConfig\":{\"type\":\"PipelineStage\",\"pipelineStageConfig\":{\"stageId\":\"1\"}}}", sharding={maximal device=0}
  stage_1_1 = f32[1,4,4,2] get-tuple-element(stage_1), index=0
  stage_1_2 = f32[1,4,4,2] get-tuple-element(stage_1), index=1
  ROOT tuple = (f32[1,4,4,2], f32[1,4,4,2]) tuple(in1, in2)
}

ENTRY e {
  e.in0 = f32[1,4,4,2] parameter(0), parameter_replication={false}
  e.in1 = f32[1,4,4,2] parameter(1), parameter_replication={false}
  e.call = (f32[1,4,4,2], f32[1,4,4,2]) call(e.in0, e.in1), to_apply=pipeline, backend_config="{\"callConfig\":{\"type\":\"Pipeline\"}}"
  gte0 = f32[1,4,4,2] get-tuple-element(e.call), index=0
  gte1 = f32[1,4,4,2] get-tuple-element(e.call), index=1
  ROOT t =  (f32[1,4,4,2], f32[1,4,4,2]) tuple(gte0, gte1)
}
)";
  auto config = GetModuleConfigForTest();
  config.set_resource_input_count(4);
  config.set_input_mapping({0, 1, 2, 3});
  config.set_resource_update_to_input_index({0, 1, 2, 3});
  TF_ASSERT_OK_AND_ASSIGN(auto module,
                          ParseAndReturnVerifiedModule(hlo, config));

  CompilerAnnotations annotations(module.get());
  ResourceUpdateVariablesOffload prvo(annotations, true, 0, 1);
  TF_ASSERT_OK_AND_ASSIGN(bool changed, prvo.Run(module.get()));
  EXPECT_FALSE(changed);
}

TEST_F(ResourceUpdateVariablesOffloadTest, DisabledByDevice) {
  std::string hlo = GetHlo(THREESTATE_ON);
  auto config = GetModuleConfigForTest();
  config.set_resource_input_count(4);
  config.set_input_mapping({0, 1, 2, 3});
  config.set_resource_update_to_input_index({0, 1, 2, 3});
  TF_ASSERT_OK_AND_ASSIGN(auto module,
                          ParseAndReturnVerifiedModule(hlo, config));

  CompilerAnnotations annotations(module.get());
  ResourceUpdateVariablesOffload prvo(annotations, false, 0, 1);
  auto status_or = prvo.Run(module.get());
  EXPECT_FALSE(status_or.ok());
  EXPECT_THAT(status_or.status().error_message(),
              ::testing::StartsWith("Current configuration of the IPU"));
}

TEST_F(ResourceUpdateVariablesOffloadTest, DisabledByDeviceDefaultConfig) {
  std::string hlo = GetHlo(THREESTATE_UNDEFINED);
  auto config = GetModuleConfigForTest();
  config.set_resource_input_count(4);
  config.set_input_mapping({0, 1, 2, 3});
  config.set_resource_update_to_input_index({0, 1, 2, 3});
  TF_ASSERT_OK_AND_ASSIGN(auto module,
                          ParseAndReturnVerifiedModule(hlo, config));

  CompilerAnnotations annotations(module.get());
  ResourceUpdateVariablesOffload prvo(annotations, false, 0, 1);
  TF_ASSERT_OK_AND_ASSIGN(bool changed, prvo.Run(module.get()));
  EXPECT_FALSE(changed);
}

TEST_F(ResourceUpdateVariablesOffloadTest, DisabledByUser) {
  std::string hlo = R"(
HloModule top

stage_0_fwd {
  in0 = f32[1,4,4,2] parameter(0)
  in1 = f32[1,4,4,2] parameter(1)
  ROOT tuple = (f32[1,4,4,2], f32[1,4,4,2]) tuple(in0, in1)
}

stage_1_fwd {
  in1 = f32[1,4,4,2] parameter(0)
  in2 = f32[1,4,4,2] parameter(1)
  ROOT tuple = (f32[1,4,4,2], f32[1,4,4,2]) tuple(in1, in2)
}

stage_1_bwd {
  in1_grad = f32[1,4,4,2] parameter(0)
  in2_grad = f32[1,4,4,2] parameter(1)
  ROOT tuple = (f32[1,4,4,2], f32[1,4,4,2]) tuple(in1_grad, in2_grad)
}

stage_0_bwd {
  in0_grad = f32[1,4,4,2] parameter(0)
  in1_grad = f32[1,4,4,2] parameter(1)
  ROOT tuple = (f32[1,4,4,2], f32[1,4,4,2]) tuple(in0_grad, in1_grad)
}

resource_update {
  arg0 = f32[1,4,4,2] parameter(0)
  arg1 = f32[1,4,4,2] parameter(1)
  arg2 = f32[1,4,4,2] parameter(2)
  arg3 = f32[1,4,4,2] parameter(3)
  arg2_new = f32[1,4,4,2] add(arg2, arg0)
  arg3_new = f32[1,4,4,2] add(arg3, arg1)
  ROOT t = (f32[1,4,4,2], f32[1,4,4,2], f32[1,4,4,2]) tuple(arg0, arg1, arg2_new, arg3_new)
}

pipeline {
  after-all = token[] after-all()
  infeed = (f32[1,4,4,2], token[]) infeed(after-all), infeed_config="140121807314576"
  in0 = f32[1,4,4,2] get-tuple-element(infeed), index=0
  in1 = f32[1,4,4,2] parameter(0)
  in2 = f32[1,4,4,2] parameter(1)
  stage_0 = (f32[1,4,4,2], f32[1,4,4,2]) call(in1, in0), to_apply=stage_0_fwd, backend_config="{\"callConfig\":{\"type\":\"PipelineStage\",\"pipelineStageConfig\":{\"stageId\":\"0\"}}}", sharding={maximal device=0}
  stage_0_0 = f32[1,4,4,2] get-tuple-element(stage_0), index=0
  stage_0_1 = f32[1,4,4,2] get-tuple-element(stage_0), index=1
  stage_1 = (f32[1,4,4,2], f32[1,4,4,2]) call(stage_0_0, stage_0_1), to_apply=stage_1_fwd, backend_config="{\"callConfig\":{\"type\":\"PipelineStage\",\"pipelineStageConfig\":{\"stageId\":\"1\"}}}", sharding={maximal device=0}
  stage_1_1 = f32[1,4,4,2] get-tuple-element(stage_1), index=0
  stage_1_2 = f32[1,4,4,2] get-tuple-element(stage_1), index=1
  stage_1_bwd = (f32[1,4,4,2], f32[1,4,4,2]) call(stage_1_1, stage_1_2), to_apply=stage_1_bwd, backend_config="{\"callConfig\":{\"type\":\"PipelineStageBackward\",\"pipelineStageConfig\":{\"stageId\":\"1\"}}}", sharding={maximal device=0}
  stage_1_bwd_1 = f32[1,4,4,2] get-tuple-element(stage_1_bwd), index=0
  stage_0_bwd = (f32[1,4,4,2], f32[1,4,4,2]) call(stage_1_bwd_1, stage_0_0), to_apply=stage_0_bwd, backend_config="{\"callConfig\":{\"type\":\"PipelineStageBackward\",\"pipelineStageConfig\":{\"stageId\":\"0\"}}}", sharding={maximal device=0}
  stage_0_bwd_0 = f32[1,4,4,2] get-tuple-element(stage_0_bwd), index=0
  in3 = f32[1,4,4,2] parameter(2)
  in4 = f32[1,4,4,2] parameter(3)
  call_ru = (f32[1,4,4,2], f32[1,4,4,2], f32[1,4,4,2], f32[1,4,4,2]) call(stage_0_bwd_0, stage_1_bwd_1, in3, in4), to_apply=resource_update, frontend_attributes={CALL_CONFIG_TYPE=ResourceUpdate}, backend_config="{\"callConfig\":{\"type\":\"ResourceUpdate\",\"resourceUpdateConfig\":{\"offloadVariables\":\"THREESTATE_OFF\"}}}"
  gte0 = f32[1,4,4,2] get-tuple-element(call_ru), index=0
  gte1 = f32[1,4,4,2] get-tuple-element(call_ru), index=1
  gte2 = f32[1,4,4,2] get-tuple-element(call_ru), index=2
  gte3 = f32[1,4,4,2] get-tuple-element(call_ru), index=3
  ROOT tuple = (f32[1,4,4,2], f32[1,4,4,2], f32[1,4,4,2], f32[1,4,4,2]) tuple(gte0, gte1, gte2, gte3)
}

ENTRY e {
  e.in0 = f32[1,4,4,2] parameter(0), parameter_replication={false}
  e.in1 = f32[1,4,4,2] parameter(1), parameter_replication={false}
  e.in2 = f32[1,4,4,2] parameter(2), parameter_replication={false}
  e.in3 = f32[1,4,4,2] parameter(3), parameter_replication={false}
  e.call = (f32[1,4,4,2], f32[1,4,4,2], f32[1,4,4,2], f32[1,4,4,2]) call(e.in0, e.in1, e.in2, e.in3), to_apply=pipeline, backend_config="{\"callConfig\":{\"type\":\"Pipeline\"}}"
  gte0 = f32[1,4,4,2] get-tuple-element(e.call), index=0
  gte1 = f32[1,4,4,2] get-tuple-element(e.call), index=1
  gte2 = f32[1,4,4,2] get-tuple-element(e.call), index=2
  gte3 = f32[1,4,4,2] get-tuple-element(e.call), index=3
  ROOT t =  (f32[1,4,4,2], f32[1,4,4,2], f32[1,4,4,2], f32[1,4,4,2]) tuple(gte0, gte1, gte2, gte3)
}
)";
  auto config = GetModuleConfigForTest();
  config.set_resource_input_count(4);
  config.set_input_mapping({0, 1, 2, 3});
  config.set_resource_update_to_input_index({0, 1, 2, 3});
  TF_ASSERT_OK_AND_ASSIGN(auto module,
                          ParseAndReturnVerifiedModule(hlo, config));

  CompilerAnnotations annotations(module.get());
  ResourceUpdateVariablesOffload prvo(annotations, true, 0, 1);
  TF_ASSERT_OK_AND_ASSIGN(bool changed, prvo.Run(module.get()));
  EXPECT_FALSE(changed);
}

TEST_F(ResourceUpdateVariablesOffloadTest, NonResourceVariables) {
  std::string hlo = R"(
HloModule top

stage_0_fwd {
  in0 = f32[1,4,4,2] parameter(0)
  in1 = f32[1,4,4,2] parameter(1)
  ROOT tuple = (f32[1,4,4,2], f32[1,4,4,2]) tuple(in0, in1)
}

stage_1_fwd {
  in1 = f32[1,4,4,2] parameter(0)
  in2 = f32[1,4,4,2] parameter(1)
  ROOT tuple = (f32[1,4,4,2], f32[1,4,4,2]) tuple(in1, in2)
}

stage_1_bwd {
  in1_grad = f32[1,4,4,2] parameter(0)
  in2_grad = f32[1,4,4,2] parameter(1)
  ROOT tuple = (f32[1,4,4,2], f32[1,4,4,2]) tuple(in1_grad, in2_grad)
}

stage_0_bwd {
  in0_grad = f32[1,4,4,2] parameter(0)
  in1_grad = f32[1,4,4,2] parameter(1)
  ROOT tuple = (f32[1,4,4,2], f32[1,4,4,2]) tuple(in0_grad, in1_grad)
}

resource_update {
  arg0 = f32[1,4,4,2] parameter(0)
  arg1 = f32[1,4,4,2] parameter(1)
  arg2 = f32[1,4,4,2] parameter(2)
  arg3 = f32[1,4,4,2] parameter(3)
  arg2_new = f32[1,4,4,2] add(arg2, arg0)
  arg3_new = f32[1,4,4,2] add(arg3, arg1)
  ROOT t = (f32[1,4,4,2], f32[1,4,4,2], f32[1,4,4,2]) tuple(arg0, arg1, arg2_new, arg3_new)
}

pipeline {
  after-all = token[] after-all()
  infeed = (f32[1,4,4,2], token[]) infeed(after-all), infeed_config="140121807314576"
  in0 = f32[1,4,4,2] get-tuple-element(infeed), index=0
  in1 = f32[1,4,4,2] parameter(0)
  in2 = f32[1,4,4,2] parameter(1)
  stage_0 = (f32[1,4,4,2], f32[1,4,4,2]) call(in1, in0), to_apply=stage_0_fwd, backend_config="{\"callConfig\":{\"type\":\"PipelineStage\",\"pipelineStageConfig\":{\"stageId\":\"0\"}}}", sharding={maximal device=0}
  stage_0_0 = f32[1,4,4,2] get-tuple-element(stage_0), index=0
  stage_0_1 = f32[1,4,4,2] get-tuple-element(stage_0), index=1
  stage_1 = (f32[1,4,4,2], f32[1,4,4,2]) call(stage_0_0, stage_0_1), to_apply=stage_1_fwd, backend_config="{\"callConfig\":{\"type\":\"PipelineStage\",\"pipelineStageConfig\":{\"stageId\":\"1\"}}}", sharding={maximal device=0}
  stage_1_1 = f32[1,4,4,2] get-tuple-element(stage_1), index=0
  stage_1_2 = f32[1,4,4,2] get-tuple-element(stage_1), index=1
  stage_1_bwd = (f32[1,4,4,2], f32[1,4,4,2]) call(stage_1_1, stage_1_2), to_apply=stage_1_bwd, backend_config="{\"callConfig\":{\"type\":\"PipelineStageBackward\",\"pipelineStageConfig\":{\"stageId\":\"1\"}}}", sharding={maximal device=0}
  stage_1_bwd_1 = f32[1,4,4,2] get-tuple-element(stage_1_bwd), index=0
  stage_0_bwd = (f32[1,4,4,2], f32[1,4,4,2]) call(stage_1_bwd_1, stage_0_0), to_apply=stage_0_bwd, backend_config="{\"callConfig\":{\"type\":\"PipelineStageBackward\",\"pipelineStageConfig\":{\"stageId\":\"0\"}}}", sharding={maximal device=0}
  stage_0_bwd_0 = f32[1,4,4,2] get-tuple-element(stage_0_bwd), index=0
  in3 = f32[1,4,4,2] parameter(2)
  in4 = f32[1,4,4,2] parameter(3)
  call_ru = (f32[1,4,4,2], f32[1,4,4,2], f32[1,4,4,2], f32[1,4,4,2]) call(stage_0_bwd_0, stage_1_bwd_1, in3, in4), to_apply=resource_update, frontend_attributes={CALL_CONFIG_TYPE=ResourceUpdate}, backend_config="{\"callConfig\":{\"type\":\"ResourceUpdate\",\"resourceUpdateConfig\":{\"offloadVariables\":\"THREESTATE_OFF\"}}}"
  gte0 = f32[1,4,4,2] get-tuple-element(call_ru), index=0
  gte1 = f32[1,4,4,2] get-tuple-element(call_ru), index=1
  gte2 = f32[1,4,4,2] get-tuple-element(call_ru), index=2
  gte3 = f32[1,4,4,2] get-tuple-element(call_ru), index=3
  ROOT tuple = (f32[1,4,4,2], f32[1,4,4,2], f32[1,4,4,2], f32[1,4,4,2]) tuple(gte0, gte1, gte2, gte3)
}

ENTRY e {
  e.in0 = f32[1,4,4,2] parameter(0), parameter_replication={false}
  e.in1 = f32[1,4,4,2] parameter(1), parameter_replication={false}
  e.in2 = f32[1,4,4,2] parameter(2), parameter_replication={false}
  e.in3 = f32[1,4,4,2] parameter(3), parameter_replication={false}
  e.call = (f32[1,4,4,2], f32[1,4,4,2], f32[1,4,4,2], f32[1,4,4,2]) call(e.in0, e.in1, e.in2, e.in3), to_apply=pipeline, backend_config="{\"callConfig\":{\"type\":\"Pipeline\"}}"
  gte0 = f32[1,4,4,2] get-tuple-element(e.call), index=0
  gte1 = f32[1,4,4,2] get-tuple-element(e.call), index=1
  gte2 = f32[1,4,4,2] get-tuple-element(e.call), index=2
  gte3 = f32[1,4,4,2] get-tuple-element(e.call), index=3
  ROOT t =  (f32[1,4,4,2], f32[1,4,4,2], f32[1,4,4,2], f32[1,4,4,2]) tuple(gte0, gte1, gte2, gte3)
}
)";
  auto config = GetModuleConfigForTest();
  // Note that unlike other tests we do not mark inputs/outputs as resources
  // here.
  TF_ASSERT_OK_AND_ASSIGN(auto module,
                          ParseAndReturnVerifiedModule(hlo, config));

  CompilerAnnotations annotations(module.get());
  ResourceUpdateVariablesOffload prvo(annotations, true, 0, 1);
  TF_ASSERT_OK_AND_ASSIGN(bool changed, prvo.Run(module.get()));
  EXPECT_FALSE(changed);
}

TEST_F(ResourceUpdateVariablesOffloadTest, PipelineInputOutputDoesntAlign) {
  std::string hlo = R"(
HloModule top

stage_0_fwd {
  in0 = f32[1,4,4,2] parameter(0)
  in1 = f32[1,4,4,2] parameter(1)
  ROOT tuple = (f32[1,4,4,2], f32[1,4,4,2]) tuple(in0, in1)
}

stage_1_fwd {
  in1 = f32[1,4,4,2] parameter(0)
  in2 = f32[1,4,4,2] parameter(1)
  ROOT tuple = (f32[1,4,4,2], f32[1,4,4,2]) tuple(in1, in2)
}

stage_1_bwd {
  in1_grad = f32[1,4,4,2] parameter(0)
  in2_grad = f32[1,4,4,2] parameter(1)
  ROOT tuple = (f32[1,4,4,2], f32[1,4,4,2]) tuple(in1_grad, in2_grad)
}

stage_0_bwd {
  in0_grad = f32[1,4,4,2] parameter(0)
  in1_grad = f32[1,4,4,2] parameter(1)
  ROOT tuple = (f32[1,4,4,2], f32[1,4,4,2]) tuple(in0_grad, in1_grad)
}

resource_update {
  arg0 = f32[1,4,4,2] parameter(0)
  arg1 = f32[1,4,4,2] parameter(1)
  arg2 = f32[1,4,4,2] parameter(2)
  arg3 = f32[1,4,4,2] parameter(3)
  arg2_new = f32[1,4,4,2] add(arg2, arg0)
  arg3_new = f32[1,4,4,2] add(arg3, arg1)
  ROOT t = (f32[1,4,4,2], f32[1,4,4,2], f32[1,4,4,2]) tuple(arg0, arg1, arg2_new, arg3_new)
}

pipeline {
  after-all = token[] after-all()
  infeed = (f32[1,4,4,2], token[]) infeed(after-all), infeed_config="140121807314576"
  in0 = f32[1,4,4,2] get-tuple-element(infeed), index=0
  in1 = f32[1,4,4,2] parameter(0)
  in2 = f32[1,4,4,2] parameter(1)
  stage_0 = (f32[1,4,4,2], f32[1,4,4,2]) call(in1, in0), to_apply=stage_0_fwd, backend_config="{\"callConfig\":{\"type\":\"PipelineStage\",\"pipelineStageConfig\":{\"stageId\":\"0\"}}}", sharding={maximal device=0}
  stage_0_0 = f32[1,4,4,2] get-tuple-element(stage_0), index=0
  stage_0_1 = f32[1,4,4,2] get-tuple-element(stage_0), index=1
  stage_1 = (f32[1,4,4,2], f32[1,4,4,2]) call(stage_0_0, stage_0_1), to_apply=stage_1_fwd, backend_config="{\"callConfig\":{\"type\":\"PipelineStage\",\"pipelineStageConfig\":{\"stageId\":\"1\"}}}", sharding={maximal device=0}
  stage_1_1 = f32[1,4,4,2] get-tuple-element(stage_1), index=0
  stage_1_2 = f32[1,4,4,2] get-tuple-element(stage_1), index=1
  stage_1_bwd = (f32[1,4,4,2], f32[1,4,4,2]) call(stage_1_1, stage_1_2), to_apply=stage_1_bwd, backend_config="{\"callConfig\":{\"type\":\"PipelineStageBackward\",\"pipelineStageConfig\":{\"stageId\":\"1\"}}}", sharding={maximal device=0}
  stage_1_bwd_1 = f32[1,4,4,2] get-tuple-element(stage_1_bwd), index=0
  stage_0_bwd = (f32[1,4,4,2], f32[1,4,4,2]) call(stage_1_bwd_1, stage_0_0), to_apply=stage_0_bwd, backend_config="{\"callConfig\":{\"type\":\"PipelineStageBackward\",\"pipelineStageConfig\":{\"stageId\":\"0\"}}}", sharding={maximal device=0}
  stage_0_bwd_0 = f32[1,4,4,2] get-tuple-element(stage_0_bwd), index=0
  in3 = f32[1,4,4,2] parameter(2)
  in4 = f32[1,4,4,2] parameter(3)
  call_ru = (f32[1,4,4,2], f32[1,4,4,2], f32[1,4,4,2], f32[1,4,4,2]) call(stage_0_bwd_0, stage_1_bwd_1, in3, in4), to_apply=resource_update, frontend_attributes={CALL_CONFIG_TYPE=ResourceUpdate}, backend_config="{\"callConfig\":{\"type\":\"ResourceUpdate\",\"resourceUpdateConfig\":{\"offloadVariables\":\"THREESTATE_OFF\"}}}"
  gte0 = f32[1,4,4,2] get-tuple-element(call_ru), index=0
  gte1 = f32[1,4,4,2] get-tuple-element(call_ru), index=1
  gte2 = f32[1,4,4,2] get-tuple-element(call_ru), index=2
  gte3 = f32[1,4,4,2] get-tuple-element(call_ru), index=3
  ROOT tuple = (f32[1,4,4,2], f32[1,4,4,2], f32[1,4,4,2], f32[1,4,4,2]) tuple(gte0, gte1, gte3, gte2)
}

ENTRY e {
  e.in0 = f32[1,4,4,2] parameter(0), parameter_replication={false}
  e.in1 = f32[1,4,4,2] parameter(1), parameter_replication={false}
  e.in2 = f32[1,4,4,2] parameter(2), parameter_replication={false}
  e.in3 = f32[1,4,4,2] parameter(3), parameter_replication={false}
  e.call = (f32[1,4,4,2], f32[1,4,4,2], f32[1,4,4,2], f32[1,4,4,2]) call(e.in0, e.in1, e.in2, e.in3), to_apply=pipeline, backend_config="{\"callConfig\":{\"type\":\"Pipeline\"}}"
  gte0 = f32[1,4,4,2] get-tuple-element(e.call), index=0
  gte1 = f32[1,4,4,2] get-tuple-element(e.call), index=1
  gte2 = f32[1,4,4,2] get-tuple-element(e.call), index=2
  gte3 = f32[1,4,4,2] get-tuple-element(e.call), index=3
  ROOT t =  (f32[1,4,4,2], f32[1,4,4,2], f32[1,4,4,2], f32[1,4,4,2]) tuple(gte0, gte1, gte2, gte3)
}
)";
  auto config = GetModuleConfigForTest();
  config.set_resource_input_count(4);
  config.set_input_mapping({0, 1, 2, 3});
  config.set_resource_update_to_input_index({0, 1, 2, 3});
  TF_ASSERT_OK_AND_ASSIGN(auto module,
                          ParseAndReturnVerifiedModule(hlo, config));

  CompilerAnnotations annotations(module.get());
  ResourceUpdateVariablesOffload prvo(annotations, true, 0, 1);
  TF_ASSERT_OK_AND_ASSIGN(bool changed, prvo.Run(module.get()));
  EXPECT_FALSE(changed);
}

// Add a test case which checks that a read-only offloaded variable is loaded,
// but no store instruction is created.
TEST_F(ResourceUpdateVariablesOffloadTest, ReadOnly) {
  std::string hlo = R"(
HloModule top

resource_update {
  arg0 = f32[1,4,4,2] parameter(0)
  arg1 = f32[1,4,4,2] parameter(1)
  arg2 = f32[1,4,4,2] parameter(2)
  arg3 = f32[1,4,4,2] parameter(3)
  arg2_new = f32[1,4,4,2] add(arg2, arg0)
  arg3_new = f32[1,4,4,2] add(arg3, arg1)
  arg3_new_new = f32[1,4,4,2] add(arg3_new, arg2_new)
  ROOT t = (f32[1,4,4,2], f32[1,4,4,2], f32[1,4,4,2]) tuple(arg0, arg1, arg2, arg3_new_new)
}

loop {
  after-all = token[] after-all()
  infeed = (f32[1,4,4,2], token[]) infeed(after-all), infeed_config="140121807314576"
  in0 = f32[1,4,4,2] get-tuple-element(infeed), index=0
  in1 = f32[1,4,4,2] parameter(0)
  add1 = f32[1,4,4,2] add(in0, in1)
  in2 = f32[1,4,4,2] parameter(1)
  add2 = f32[1,4,4,2] add(in0, in2)
  in3 = f32[1,4,4,2] parameter(2)
  in4 = f32[1,4,4,2] parameter(3)
  call_ru = (f32[1,4,4,2], f32[1,4,4,2], f32[1,4,4,2], f32[1,4,4,2]) call(add1, add2, in3, in4), to_apply=resource_update, frontend_attributes={CALL_CONFIG_TYPE=ResourceUpdate}, backend_config="{\"callConfig\":{\"type\":\"ResourceUpdate\",\"resourceUpdateConfig\":{\"offloadVariables\":\"THREESTATE_ON\"}}}"
  gte0 = f32[1,4,4,2] get-tuple-element(call_ru), index=0
  gte1 = f32[1,4,4,2] get-tuple-element(call_ru), index=1
  gte2 = f32[1,4,4,2] get-tuple-element(call_ru), index=2
  gte3 = f32[1,4,4,2] get-tuple-element(call_ru), index=3
  ROOT tuple = (f32[1,4,4,2], f32[1,4,4,2], f32[1,4,4,2], f32[1,4,4,2]) tuple(gte0, gte1, gte2, gte3)
}

ENTRY e {
  e.in0 = f32[1,4,4,2] parameter(0), parameter_replication={false}
  e.in1 = f32[1,4,4,2] parameter(1), parameter_replication={false}
  e.in2 = f32[1,4,4,2] parameter(2), parameter_replication={false}
  e.in3 = f32[1,4,4,2] parameter(3), parameter_replication={false}
  e.call = (f32[1,4,4,2], f32[1,4,4,2], f32[1,4,4,2], f32[1,4,4,2]) call(e.in0, e.in1, e.in2, e.in3), to_apply=loop, backend_config="{\"callConfig\":{\"type\":\"RepeatLoop\",\"repeatConfig\":{\"repeatCount\":\"100\"}}}"
  gte0 = f32[1,4,4,2] get-tuple-element(e.call), index=0
  gte1 = f32[1,4,4,2] get-tuple-element(e.call), index=1
  gte2 = f32[1,4,4,2] get-tuple-element(e.call), index=2
  gte3 = f32[1,4,4,2] get-tuple-element(e.call), index=3
  ROOT t =  (f32[1,4,4,2], f32[1,4,4,2], f32[1,4,4,2], f32[1,4,4,2]) tuple(gte0, gte1, gte2, gte3)
}
)";
  auto config = GetModuleConfigForTest();
  config.set_resource_input_count(4);
  config.set_input_mapping({0, 1, 2, 3});
  config.set_resource_update_to_input_index({0, 1, 2, 3});
  TF_ASSERT_OK_AND_ASSIGN(auto module,
                          ParseAndReturnVerifiedModule(hlo, config));

  CompilerAnnotations annotations(module.get());
  ResourceUpdateVariablesOffload prvo(annotations, true, 0, 1);
  TF_ASSERT_OK_AND_ASSIGN(bool changed, prvo.Run(module.get()));
  EXPECT_TRUE(changed);

  auto root = module->entry_computation()->root_instruction();
  HloComputation* repeat_computation = root->operand(0)->operand(0)->to_apply();
  HloInstruction* repeat_root = repeat_computation->root_instruction();
  HloComputation* resource_update =
      repeat_root->mutable_operand(0)->mutable_operand(0)->to_apply();
  EXPECT_EQ(resource_update->num_parameters(), 4);
  EXPECT_EQ(ShapeUtil::TupleElementCount(
                resource_update->root_instruction()->shape()),
            4);

  // Check there is 1 store and 2 loads.
  auto insts = resource_update->instructions();
  EXPECT_EQ(absl::c_count_if(
                insts, IsPoplarInstruction(PoplarOp::RemoteParameterStore)),
            1);
  EXPECT_EQ(absl::c_count_if(
                insts, IsPoplarInstruction(PoplarOp::RemoteParameterLoad)),
            2);

  HloInstruction* resource_update_root = resource_update->root_instruction();

  // Expect the first three inputs to be pass-through.
  for (int i : {0, 1, 2}) {
    const HloInstruction* operand = resource_update_root->operand(i);
    EXPECT_EQ(operand->opcode(), HloOpcode::kParameter);
  }

  // The final input should be updated with a store.
  const HloInstruction* final_operand = resource_update_root->operand(3);
  EXPECT_EQ(final_operand->opcode(), HloOpcode::kCustomCall);
  EXPECT_TRUE(
      IsPoplarInstruction(PoplarOp::RemoteParameterStore)(final_operand));
}

TEST_F(ResourceUpdateVariablesOffloadTest, OffloadVariableMinimumSize) {
  std::string hlo = R"(
HloModule top

resource_update {
  arg0 = f32[1,4,4,2] parameter(0)
  arg1 = f32[1,4,4,2] parameter(1)
  arg2 = f32[1,4,4,2] parameter(2)
  arg3 = f32[2,4,4,2] parameter(3)
  arg2_new = f32[1,4,4,2] add(arg2, arg0)
  concat = f32[2,4,4,2] concatenate(arg1, arg1), dimensions={0}
  arg3_new = f32[2,4,4,2] add(arg3, concat)
  ROOT t = (f32[1,4,4,2], f32[1,4,4,2], f32[1,4,4,2], f32[2,4,4,2]) tuple(arg0, arg1, arg2_new, arg3_new)
}

loop {
  after-all = token[] after-all()
  infeed = (f32[1,4,4,2], token[]) infeed(after-all), infeed_config="140121807314576"
  in0 = f32[1,4,4,2] get-tuple-element(infeed), index=0
  in1 = f32[1,4,4,2] parameter(0)
  add1 = f32[1,4,4,2] add(in0, in1)
  in2 = f32[1,4,4,2] parameter(1)
  add2 = f32[1,4,4,2] add(in0, in2)
  in3 = f32[1,4,4,2] parameter(2)
  in4 = f32[2,4,4,2] parameter(3)
  call_ru = (f32[1,4,4,2], f32[1,4,4,2], f32[1,4,4,2], f32[2,4,4,2]) call(add1, add2, in3, in4), to_apply=resource_update, frontend_attributes={CALL_CONFIG_TYPE=ResourceUpdate}, backend_config="{\"callConfig\":{\"type\":\"ResourceUpdate\",\"resourceUpdateConfig\":{\"offloadVariables\":\"THREESTATE_ON\"}}}"
  gte0 = f32[1,4,4,2] get-tuple-element(call_ru), index=0
  gte1 = f32[1,4,4,2] get-tuple-element(call_ru), index=1
  gte2 = f32[1,4,4,2] get-tuple-element(call_ru), index=2
  gte3 = f32[2,4,4,2] get-tuple-element(call_ru), index=3
  ROOT tuple = (f32[1,4,4,2], f32[1,4,4,2], f32[1,4,4,2], f32[2,4,4,2]) tuple(gte0, gte1, gte2, gte3)
}

ENTRY e {
  e.in0 = f32[1,4,4,2] parameter(0), parameter_replication={false}
  e.in1 = f32[1,4,4,2] parameter(1), parameter_replication={false}
  e.in2 = f32[1,4,4,2] parameter(2), parameter_replication={false}
  e.in3 = f32[2,4,4,2] parameter(3), parameter_replication={false}
  e.call = (f32[1,4,4,2], f32[1,4,4,2], f32[1,4,4,2], f32[2,4,4,2]) call(e.in0, e.in1, e.in2, e.in3), to_apply=loop, backend_config="{\"callConfig\":{\"type\":\"RepeatLoop\",\"repeatConfig\":{\"repeatCount\":\"100\"}}}"
  gte0 = f32[1,4,4,2] get-tuple-element(e.call), index=0
  gte1 = f32[1,4,4,2] get-tuple-element(e.call), index=1
  gte2 = f32[1,4,4,2] get-tuple-element(e.call), index=2
  gte3 = f32[2,4,4,2] get-tuple-element(e.call), index=3
  ROOT t =  (f32[1,4,4,2], f32[1,4,4,2], f32[1,4,4,2], f32[2,4,4,2]) tuple(gte0, gte1, gte2, gte3)
}
)";
  auto config = GetModuleConfigForTest();
  config.set_resource_input_count(4);
  config.set_input_mapping({0, 1, 2, 3});
  config.set_resource_update_to_input_index({0, 1, 2, 3});
  TF_ASSERT_OK_AND_ASSIGN(auto module,
                          ParseAndReturnVerifiedModule(hlo, config));

  CompilerAnnotations annotations(module.get());
  ResourceUpdateVariablesOffload prvo(annotations, true, 129, 1);
  TF_ASSERT_OK_AND_ASSIGN(bool changed, prvo.Run(module.get()));
  EXPECT_TRUE(changed);
  auto root = module->entry_computation()->root_instruction();
  HloComputation* repeat_computation = root->operand(0)->operand(0)->to_apply();
  HloInstruction* repeat_root = repeat_computation->root_instruction();
  HloComputation* resource_update =
      repeat_root->mutable_operand(0)->mutable_operand(0)->to_apply();
  EXPECT_EQ(resource_update->num_parameters(), 4);
  EXPECT_EQ(ShapeUtil::TupleElementCount(
                resource_update->root_instruction()->shape()),
            4);

  // Check load and stores.
  HloInstruction* param = resource_update->parameter_instruction(3);
  ASSERT_EQ(param->user_count(), 2);
  HloInstruction *load, *store;
  if (IsPoplarInstruction(PoplarOp::RemoteParameterLoad)(param->users()[0])) {
    load = param->users()[0];
    store = param->users()[1];
  } else {
    load = param->users()[1];
    store = param->users()[0];
  }

  EXPECT_TRUE(IsPoplarInstruction(PoplarOp::RemoteParameterLoad)(load));
  EXPECT_TRUE(IsPoplarInstruction(PoplarOp::RemoteParameterStore)(store));

  ASSERT_EQ(load->user_count(), 1);
  HloInstruction* add = load->users()[0];
  EXPECT_EQ(add->opcode(), HloOpcode::kAdd);
  ASSERT_EQ(add->user_count(), 1);
  EXPECT_EQ(add->users()[0], store);

  // Check other parameter was not offloaded as it is too small.
  param = resource_update->parameter_instruction(2);
  ASSERT_EQ(param->user_count(), 1);
  add = param->users()[0];
  EXPECT_EQ(add->opcode(), HloOpcode::kAdd);
}

TEST_F(ResourceUpdateVariablesOffloadTest, OffloadVariableReplicated) {
  std::string hlo = R"(
HloModule top

stage_0_fwd {
  in0 = f32[1,4,4,2] parameter(0)
  in1 = f32[1,4,4,2] parameter(1)
  ROOT tuple = (f32[1,4,4,2], f32[1,4,4,2]) tuple(in0, in1)
}

stage_1_fwd {
  in1 = f32[1,4,4,2] parameter(0)
  in2 = f32[1,4,4,2] parameter(1)
  ROOT tuple = (f32[1,4,4,2], f32[1,4,4,2]) tuple(in1, in2)
}

stage_1_bwd {
  in1_grad = f32[1,4,4,2] parameter(0)
  in2_grad = f32[1,4,4,2] parameter(1)
  ROOT tuple = (f32[1,4,4,2], f32[1,4,4,2]) tuple(in1_grad, in2_grad)
}

stage_0_bwd {
  in0_grad = f32[1,4,4,2] parameter(0)
  in1_grad = f32[1,4,4,2] parameter(1)
  ROOT tuple = (f32[1,4,4,2], f32[1,4,4,2]) tuple(in0_grad, in1_grad)
}

resource_update {
  arg0 = f32[1,4,4,2] parameter(0)
  arg1 = f32[1,4,4,2] parameter(1)
  arg2 = f32[1,4,4,2] parameter(2)
  arg3 = f32[1,4,4,2] parameter(3)
  arg2_new = f32[1,4,4,2] add(arg2, arg0)
  arg3_new = f32[1,4,4,2] add(arg3, arg1)
  ROOT t = (f32[1,4,4,2], f32[1,4,4,2], f32[1,4,4,2]) tuple(arg0, arg1, arg2_new, arg3_new)
}

pipeline {
  after-all = token[] after-all()
  infeed = (f32[1,4,4,2], token[]) infeed(after-all), infeed_config="140121807314576"
  in0 = f32[1,4,4,2] get-tuple-element(infeed), index=0
  in1 = f32[1,4,4,2] parameter(0)
  in2 = f32[1,4,4,2] parameter(1)
  stage_0 = (f32[1,4,4,2], f32[1,4,4,2]) call(in1, in0), to_apply=stage_0_fwd, backend_config="{\"callConfig\":{\"type\":\"PipelineStage\",\"pipelineStageConfig\":{\"stageId\":\"0\"}}}", sharding={maximal device=0}
  stage_0_0 = f32[1,4,4,2] get-tuple-element(stage_0), index=0
  stage_0_1 = f32[1,4,4,2] get-tuple-element(stage_0), index=1
  stage_1 = (f32[1,4,4,2], f32[1,4,4,2]) call(stage_0_0, stage_0_1), to_apply=stage_1_fwd, backend_config="{\"callConfig\":{\"type\":\"PipelineStage\",\"pipelineStageConfig\":{\"stageId\":\"1\"}}}", sharding={maximal device=0}
  stage_1_1 = f32[1,4,4,2] get-tuple-element(stage_1), index=0
  stage_1_2 = f32[1,4,4,2] get-tuple-element(stage_1), index=1
  stage_1_bwd = (f32[1,4,4,2], f32[1,4,4,2]) call(stage_1_1, stage_1_2), to_apply=stage_1_bwd, backend_config="{\"callConfig\":{\"type\":\"PipelineStageBackward\",\"pipelineStageConfig\":{\"stageId\":\"1\"}}}", sharding={maximal device=0}
  stage_1_bwd_1 = f32[1,4,4,2] get-tuple-element(stage_1_bwd), index=0
  stage_0_bwd = (f32[1,4,4,2], f32[1,4,4,2]) call(stage_1_bwd_1, stage_0_0), to_apply=stage_0_bwd, backend_config="{\"callConfig\":{\"type\":\"PipelineStageBackward\",\"pipelineStageConfig\":{\"stageId\":\"0\"}}}", sharding={maximal device=0}
  stage_0_bwd_0 = f32[1,4,4,2] get-tuple-element(stage_0_bwd), index=0
  in3 = f32[1,4,4,2] parameter(2)
  in4 = f32[1,4,4,2] parameter(3)
  call_ru = (f32[1,4,4,2], f32[1,4,4,2], f32[1,4,4,2], f32[1,4,4,2]) call(stage_0_bwd_0, stage_1_bwd_1, in3, in4), to_apply=resource_update, frontend_attributes={CALL_CONFIG_TYPE=ResourceUpdate}, backend_config="{\"callConfig\":{\"type\":\"ResourceUpdate\",\"resourceUpdateConfig\":{\"offloadVariables\":\"THREESTATE_ON\", \"partitionOffloadedVariables\":\"THREESTATE_ON\"}}}"
  gte0 = f32[1,4,4,2] get-tuple-element(call_ru), index=0
  gte1 = f32[1,4,4,2] get-tuple-element(call_ru), index=1
  gte2 = f32[1,4,4,2] get-tuple-element(call_ru), index=2
  gte3 = f32[1,4,4,2] get-tuple-element(call_ru), index=3
  ROOT tuple = (f32[1,4,4,2], f32[1,4,4,2], f32[1,4,4,2], f32[1,4,4,2]) tuple(gte0, gte1, gte2, gte3)
}

ENTRY e {
  e.in0 = f32[1,4,4,2] parameter(0), parameter_replication={false}
  e.in1 = f32[1,4,4,2] parameter(1), parameter_replication={false}
  e.in2 = f32[1,4,4,2] parameter(2), parameter_replication={false}
  e.in3 = f32[1,4,4,2] parameter(3), parameter_replication={false}
  e.call = (f32[1,4,4,2], f32[1,4,4,2], f32[1,4,4,2], f32[1,4,4,2]) call(e.in0, e.in1, e.in2, e.in3), to_apply=pipeline, backend_config="{\"callConfig\":{\"type\":\"Pipeline\"}}"
  gte0 = f32[1,4,4,2] get-tuple-element(e.call), index=0
  gte1 = f32[1,4,4,2] get-tuple-element(e.call), index=1
  gte2 = f32[1,4,4,2] get-tuple-element(e.call), index=2
  gte3 = f32[1,4,4,2] get-tuple-element(e.call), index=3
  ROOT t =  (f32[1,4,4,2], f32[1,4,4,2], f32[1,4,4,2], f32[1,4,4,2]) tuple(gte0, gte1, gte2, gte3)
}
)";
  auto config = GetModuleConfigForTest();
  config.set_resource_input_count(4);
  config.set_input_mapping({0, 1, 2, 3});
  config.set_resource_update_to_input_index({0, 1, 2, 3});
  TF_ASSERT_OK_AND_ASSIGN(auto module,
                          ParseAndReturnVerifiedModule(hlo, config));

  CompilerAnnotations annotations(module.get());
  ResourceUpdateVariablesOffload prvo(annotations, true, 0, 2);
  TF_ASSERT_OK_AND_ASSIGN(bool changed, prvo.Run(module.get()));
  EXPECT_TRUE(changed);
  auto root = module->entry_computation()->root_instruction();
  HloComputation* pipeline_computation =
      root->operand(0)->operand(0)->to_apply();
  TF_ASSERT_OK_AND_ASSIGN(auto stages, GetPipelineStages(pipeline_computation));
  HloComputation* resource_update = (*stages.resource_update)->to_apply();
  EXPECT_EQ(resource_update->num_parameters(), 4);
  EXPECT_EQ(ShapeUtil::TupleElementCount(
                resource_update->root_instruction()->shape()),
            4);

  // Check load and stores.
  for (int64 i : {0, 1}) {
    HloInstruction* param = resource_update->parameter_instruction(i);
    EXPECT_EQ(param->user_count(), 2);
    HloInstruction* add = nullptr;
    for (auto* user : param->users()) {
      if (user->opcode() == HloOpcode::kAdd) {
        add = user;
        break;
      }
    }
    EXPECT_TRUE(add);
    const HloInstruction* reshape = add->operand(0);
    EXPECT_TRUE(reshape->opcode() == HloOpcode::kReshape);
    const HloInstruction* all_gather = reshape->operand(0);
    EXPECT_TRUE(IsPoplarInstruction(PoplarOp::AllGather)(all_gather));
    const HloInstruction* load = all_gather->operand(0);
    EXPECT_TRUE(IsPoplarInstruction(PoplarOp::RemoteParameterLoad)(load));

    EXPECT_EQ(add->user_count(), 1);
    const HloInstruction* reshape_1 = add->users()[0];
    EXPECT_TRUE(reshape_1->opcode() == HloOpcode::kReshape);
    const HloInstruction* dynamic_slice = reshape_1->users()[0];
    EXPECT_TRUE(dynamic_slice->opcode() == HloOpcode::kDynamicSlice);
    const HloInstruction* reshape_2 = dynamic_slice->users()[0];
    EXPECT_TRUE(reshape_2->opcode() == HloOpcode::kReshape);
    const HloInstruction* store = reshape_2->users()[0];
    EXPECT_TRUE(IsPoplarInstruction(PoplarOp::RemoteParameterStore)(store));
  }
}

TEST_F(ResourceUpdateVariablesOffloadTest,
       OffloadVariableReplicatedNoPartition) {
  std::string hlo = R"(
HloModule top

stage_0_fwd {
  in0 = f32[1,4,4,2] parameter(0)
  in1 = f32[1,4,4,2] parameter(1)
  ROOT tuple = (f32[1,4,4,2], f32[1,4,4,2]) tuple(in0, in1)
}

stage_1_fwd {
  in1 = f32[1,4,4,2] parameter(0)
  in2 = f32[1,4,4,2] parameter(1)
  ROOT tuple = (f32[1,4,4,2], f32[1,4,4,2]) tuple(in1, in2)
}

stage_1_bwd {
  in1_grad = f32[1,4,4,2] parameter(0)
  in2_grad = f32[1,4,4,2] parameter(1)
  ROOT tuple = (f32[1,4,4,2], f32[1,4,4,2]) tuple(in1_grad, in2_grad)
}

stage_0_bwd {
  in0_grad = f32[1,4,4,2] parameter(0)
  in1_grad = f32[1,4,4,2] parameter(1)
  ROOT tuple = (f32[1,4,4,2], f32[1,4,4,2]) tuple(in0_grad, in1_grad)
}

resource_update {
  arg0 = f32[1,4,4,2] parameter(0)
  arg1 = f32[1,4,4,2] parameter(1)
  arg2 = f32[1,4,4,2] parameter(2)
  arg3 = f32[1,4,4,2] parameter(3)
  arg2_new = f32[1,4,4,2] add(arg2, arg0)
  arg3_new = f32[1,4,4,2] add(arg3, arg1)
  ROOT t = (f32[1,4,4,2], f32[1,4,4,2], f32[1,4,4,2]) tuple(arg0, arg1, arg2_new, arg3_new)
}

pipeline {
  after-all = token[] after-all()
  infeed = (f32[1,4,4,2], token[]) infeed(after-all), infeed_config="140121807314576"
  in0 = f32[1,4,4,2] get-tuple-element(infeed), index=0
  in1 = f32[1,4,4,2] parameter(0)
  in2 = f32[1,4,4,2] parameter(1)
  stage_0 = (f32[1,4,4,2], f32[1,4,4,2]) call(in1, in0), to_apply=stage_0_fwd, backend_config="{\"callConfig\":{\"type\":\"PipelineStage\",\"pipelineStageConfig\":{\"stageId\":\"0\"}}}", sharding={maximal device=0}
  stage_0_0 = f32[1,4,4,2] get-tuple-element(stage_0), index=0
  stage_0_1 = f32[1,4,4,2] get-tuple-element(stage_0), index=1
  stage_1 = (f32[1,4,4,2], f32[1,4,4,2]) call(stage_0_0, stage_0_1), to_apply=stage_1_fwd, backend_config="{\"callConfig\":{\"type\":\"PipelineStage\",\"pipelineStageConfig\":{\"stageId\":\"1\"}}}", sharding={maximal device=0}
  stage_1_1 = f32[1,4,4,2] get-tuple-element(stage_1), index=0
  stage_1_2 = f32[1,4,4,2] get-tuple-element(stage_1), index=1
  stage_1_bwd = (f32[1,4,4,2], f32[1,4,4,2]) call(stage_1_1, stage_1_2), to_apply=stage_1_bwd, backend_config="{\"callConfig\":{\"type\":\"PipelineStageBackward\",\"pipelineStageConfig\":{\"stageId\":\"1\"}}}", sharding={maximal device=0}
  stage_1_bwd_1 = f32[1,4,4,2] get-tuple-element(stage_1_bwd), index=0
  stage_0_bwd = (f32[1,4,4,2], f32[1,4,4,2]) call(stage_1_bwd_1, stage_0_0), to_apply=stage_0_bwd, backend_config="{\"callConfig\":{\"type\":\"PipelineStageBackward\",\"pipelineStageConfig\":{\"stageId\":\"0\"}}}", sharding={maximal device=0}
  stage_0_bwd_0 = f32[1,4,4,2] get-tuple-element(stage_0_bwd), index=0
  in3 = f32[1,4,4,2] parameter(2)
  in4 = f32[1,4,4,2] parameter(3)
  call_ru = (f32[1,4,4,2], f32[1,4,4,2], f32[1,4,4,2], f32[1,4,4,2]) call(stage_0_bwd_0, stage_1_bwd_1, in3, in4), to_apply=resource_update, frontend_attributes={CALL_CONFIG_TYPE=ResourceUpdate}, backend_config="{\"callConfig\":{\"type\":\"ResourceUpdate\",\"resourceUpdateConfig\":{\"offloadVariables\":\"THREESTATE_ON\", \"partitionOffloadedVariables\":\"THREESTATE_OFF\"}}}"
  gte0 = f32[1,4,4,2] get-tuple-element(call_ru), index=0
  gte1 = f32[1,4,4,2] get-tuple-element(call_ru), index=1
  gte2 = f32[1,4,4,2] get-tuple-element(call_ru), index=2
  gte3 = f32[1,4,4,2] get-tuple-element(call_ru), index=3
  ROOT tuple = (f32[1,4,4,2], f32[1,4,4,2], f32[1,4,4,2], f32[1,4,4,2]) tuple(gte0, gte1, gte2, gte3)
}

ENTRY e {
  e.in0 = f32[1,4,4,2] parameter(0), parameter_replication={false}
  e.in1 = f32[1,4,4,2] parameter(1), parameter_replication={false}
  e.in2 = f32[1,4,4,2] parameter(2), parameter_replication={false}
  e.in3 = f32[1,4,4,2] parameter(3), parameter_replication={false}
  e.call = (f32[1,4,4,2], f32[1,4,4,2], f32[1,4,4,2], f32[1,4,4,2]) call(e.in0, e.in1, e.in2, e.in3), to_apply=pipeline, backend_config="{\"callConfig\":{\"type\":\"Pipeline\"}}"
  gte0 = f32[1,4,4,2] get-tuple-element(e.call), index=0
  gte1 = f32[1,4,4,2] get-tuple-element(e.call), index=1
  gte2 = f32[1,4,4,2] get-tuple-element(e.call), index=2
  gte3 = f32[1,4,4,2] get-tuple-element(e.call), index=3
  ROOT t =  (f32[1,4,4,2], f32[1,4,4,2], f32[1,4,4,2], f32[1,4,4,2]) tuple(gte0, gte1, gte2, gte3)
}
)";
  auto config = GetModuleConfigForTest();
  config.set_resource_input_count(4);
  config.set_input_mapping({0, 1, 2, 3});
  config.set_resource_update_to_input_index({0, 1, 2, 3});
  TF_ASSERT_OK_AND_ASSIGN(auto module,
                          ParseAndReturnVerifiedModule(hlo, config));

  CompilerAnnotations annotations(module.get());
  ResourceUpdateVariablesOffload prvo(annotations, true, 0, 2);
  TF_ASSERT_OK_AND_ASSIGN(bool changed, prvo.Run(module.get()));
  EXPECT_TRUE(changed);
  auto root = module->entry_computation()->root_instruction();
  HloComputation* pipeline_computation =
      root->operand(0)->operand(0)->to_apply();
  TF_ASSERT_OK_AND_ASSIGN(auto stages, GetPipelineStages(pipeline_computation));
  HloComputation* resource_update = (*stages.resource_update)->to_apply();
  EXPECT_EQ(resource_update->num_parameters(), 4);
  EXPECT_EQ(ShapeUtil::TupleElementCount(
                resource_update->root_instruction()->shape()),
            4);

  // Check load and stores.
  for (int64 i : {0, 1}) {
    HloInstruction* param = resource_update->parameter_instruction(i);
    EXPECT_EQ(param->user_count(), 2);
    HloInstruction* add = nullptr;
    for (auto* user : param->users()) {
      if (user->opcode() == HloOpcode::kAdd) {
        add = user;
        break;
      }
    }
    EXPECT_TRUE(add);
    const HloInstruction* load = add->operand(0);
    EXPECT_TRUE(IsPoplarInstruction(PoplarOp::RemoteParameterLoad)(load));
    EXPECT_EQ(add->user_count(), 1);
    const HloInstruction* store = add->users()[0];
    EXPECT_TRUE(IsPoplarInstruction(PoplarOp::RemoteParameterStore)(store));
  }
}

TEST_F(ResourceUpdateVariablesOffloadTest, OffloadVariableReplicatedPadding) {
  std::string hlo = R"(
HloModule top

stage_0_fwd {
  in0 = f32[1,5,5,3] parameter(0)
  in1 = f32[1,5,5,3] parameter(1)
  ROOT tuple = (f32[1,5,5,3], f32[1,5,5,3]) tuple(in0, in1)
}

stage_1_fwd {
  in1 = f32[1,5,5,3] parameter(0)
  in2 = f32[1,5,5,3] parameter(1)
  ROOT tuple = (f32[1,5,5,3], f32[1,5,5,3]) tuple(in1, in2)
}

stage_1_bwd {
  in1_grad = f32[1,5,5,3] parameter(0)
  in2_grad = f32[1,5,5,3] parameter(1)
  ROOT tuple = (f32[1,5,5,3], f32[1,5,5,3]) tuple(in1_grad, in2_grad)
}

stage_0_bwd {
  in0_grad = f32[1,5,5,3] parameter(0)
  in1_grad = f32[1,5,5,3] parameter(1)
  ROOT tuple = (f32[1,5,5,3], f32[1,5,5,3]) tuple(in0_grad, in1_grad)
}

resource_update {
  arg0 = f32[1,5,5,3] parameter(0)
  arg1 = f32[1,5,5,3] parameter(1)
  arg2 = f32[1,5,5,3] parameter(2)
  arg3 = f32[1,5,5,3] parameter(3)
  arg2_new = f32[1,5,5,3] add(arg2, arg0)
  arg3_new = f32[1,5,5,3] add(arg3, arg1)
  ROOT t = (f32[1,5,5,3], f32[1,5,5,3], f32[1,5,5,3]) tuple(arg0, arg1, arg2_new, arg3_new)
}

pipeline {
  after-all = token[] after-all()
  infeed = (f32[1,5,5,3], token[]) infeed(after-all), infeed_config="140121807314576"
  in0 = f32[1,5,5,3] get-tuple-element(infeed), index=0
  in1 = f32[1,5,5,3] parameter(0)
  in2 = f32[1,5,5,3] parameter(1)
  stage_0 = (f32[1,5,5,3], f32[1,5,5,3]) call(in1, in0), to_apply=stage_0_fwd, backend_config="{\"callConfig\":{\"type\":\"PipelineStage\",\"pipelineStageConfig\":{\"stageId\":\"0\"}}}", sharding={maximal device=0}
  stage_0_0 = f32[1,5,5,3] get-tuple-element(stage_0), index=0
  stage_0_1 = f32[1,5,5,3] get-tuple-element(stage_0), index=1
  stage_1 = (f32[1,5,5,3], f32[1,5,5,3]) call(stage_0_0, stage_0_1), to_apply=stage_1_fwd, backend_config="{\"callConfig\":{\"type\":\"PipelineStage\",\"pipelineStageConfig\":{\"stageId\":\"1\"}}}", sharding={maximal device=0}
  stage_1_1 = f32[1,5,5,3] get-tuple-element(stage_1), index=0
  stage_1_2 = f32[1,5,5,3] get-tuple-element(stage_1), index=1
  stage_1_bwd = (f32[1,5,5,3], f32[1,5,5,3]) call(stage_1_1, stage_1_2), to_apply=stage_1_bwd, backend_config="{\"callConfig\":{\"type\":\"PipelineStageBackward\",\"pipelineStageConfig\":{\"stageId\":\"1\"}}}", sharding={maximal device=0}
  stage_1_bwd_1 = f32[1,5,5,3] get-tuple-element(stage_1_bwd), index=0
  stage_0_bwd = (f32[1,5,5,3], f32[1,5,5,3]) call(stage_1_bwd_1, stage_0_0), to_apply=stage_0_bwd, backend_config="{\"callConfig\":{\"type\":\"PipelineStageBackward\",\"pipelineStageConfig\":{\"stageId\":\"0\"}}}", sharding={maximal device=0}
  stage_0_bwd_0 = f32[1,5,5,3] get-tuple-element(stage_0_bwd), index=0
  in3 = f32[1,5,5,3] parameter(2)
  in4 = f32[1,5,5,3] parameter(3)
  call_ru = (f32[1,5,5,3], f32[1,5,5,3], f32[1,5,5,3], f32[1,5,5,3]) call(stage_0_bwd_0, stage_1_bwd_1, in3, in4), to_apply=resource_update, frontend_attributes={CALL_CONFIG_TYPE=ResourceUpdate}, backend_config="{\"callConfig\":{\"type\":\"ResourceUpdate\",\"resourceUpdateConfig\":{\"offloadVariables\":\"THREESTATE_ON\", \"partitionOffloadedVariables\":\"THREESTATE_ON\"}}}"
  gte0 = f32[1,5,5,3] get-tuple-element(call_ru), index=0
  gte1 = f32[1,5,5,3] get-tuple-element(call_ru), index=1
  gte2 = f32[1,5,5,3] get-tuple-element(call_ru), index=2
  gte3 = f32[1,5,5,3] get-tuple-element(call_ru), index=3
  ROOT tuple = (f32[1,5,5,3], f32[1,5,5,3], f32[1,5,5,3], f32[1,5,5,3]) tuple(gte0, gte1, gte2, gte3)
}

ENTRY e {
  e.in0 = f32[1,5,5,3] parameter(0), parameter_replication={false}
  e.in1 = f32[1,5,5,3] parameter(1), parameter_replication={false}
  e.in2 = f32[1,5,5,3] parameter(2), parameter_replication={false}
  e.in3 = f32[1,5,5,3] parameter(3), parameter_replication={false}
  e.call = (f32[1,5,5,3], f32[1,5,5,3], f32[1,5,5,3], f32[1,5,5,3]) call(e.in0, e.in1, e.in2, e.in3), to_apply=pipeline, backend_config="{\"callConfig\":{\"type\":\"Pipeline\"}}"
  gte0 = f32[1,5,5,3] get-tuple-element(e.call), index=0
  gte1 = f32[1,5,5,3] get-tuple-element(e.call), index=1
  gte2 = f32[1,5,5,3] get-tuple-element(e.call), index=2
  gte3 = f32[1,5,5,3] get-tuple-element(e.call), index=3
  ROOT t =  (f32[1,5,5,3], f32[1,5,5,3], f32[1,5,5,3], f32[1,5,5,3]) tuple(gte0, gte1, gte2, gte3)
}
)";
  auto config = GetModuleConfigForTest();
  config.set_resource_input_count(4);
  config.set_input_mapping({0, 1, 2, 3});
  config.set_resource_update_to_input_index({0, 1, 2, 3});
  TF_ASSERT_OK_AND_ASSIGN(auto module,
                          ParseAndReturnVerifiedModule(hlo, config));

  CompilerAnnotations annotations(module.get());
  ResourceUpdateVariablesOffload prvo(annotations, true, 0, 2);
  TF_ASSERT_OK_AND_ASSIGN(bool changed, prvo.Run(module.get()));
  EXPECT_TRUE(changed);
  auto root = module->entry_computation()->root_instruction();
  HloComputation* pipeline_computation =
      root->operand(0)->operand(0)->to_apply();
  TF_ASSERT_OK_AND_ASSIGN(auto stages, GetPipelineStages(pipeline_computation));
  HloComputation* resource_update = (*stages.resource_update)->to_apply();
  EXPECT_EQ(resource_update->num_parameters(), 4);
  EXPECT_EQ(ShapeUtil::TupleElementCount(
                resource_update->root_instruction()->shape()),
            4);

  // Check load and stores.
  for (int64 i : {0, 1}) {
    HloInstruction* param = resource_update->parameter_instruction(i);
    EXPECT_EQ(param->user_count(), 2);
    HloInstruction* add = nullptr;
    for (auto* user : param->users()) {
      if (user->opcode() == HloOpcode::kAdd) {
        add = user;
        break;
      }
    }
    EXPECT_TRUE(add);
    const HloInstruction* reshape = add->operand(0);
    EXPECT_TRUE(reshape->opcode() == HloOpcode::kReshape);
    const HloInstruction* slice = reshape->operand(0);
    EXPECT_TRUE(slice->opcode() == HloOpcode::kSlice);
    const HloInstruction* flatten = slice->operand(0);
    EXPECT_TRUE(flatten->opcode() == HloOpcode::kReshape);
    const HloInstruction* all_gather = flatten->operand(0);
    EXPECT_TRUE(IsPoplarInstruction(PoplarOp::AllGather)(all_gather));
    const HloInstruction* load = all_gather->operand(0);
    EXPECT_TRUE(IsPoplarInstruction(PoplarOp::RemoteParameterLoad)(load));

    EXPECT_EQ(add->user_count(), 1);
    const HloInstruction* flatten_2 = add->users()[0];
    EXPECT_TRUE(flatten_2->opcode() == HloOpcode::kReshape);
    const HloInstruction* pad = flatten_2->users()[0];
    EXPECT_TRUE(pad->opcode() == HloOpcode::kPad);
    const HloInstruction* reshape_1 = pad->users()[0];
    EXPECT_TRUE(reshape_1->opcode() == HloOpcode::kReshape);
    const HloInstruction* dynamic_slice = reshape_1->users()[0];
    EXPECT_TRUE(dynamic_slice->opcode() == HloOpcode::kDynamicSlice);
    const HloInstruction* reshape_2 = dynamic_slice->users()[0];
    EXPECT_TRUE(reshape_2->opcode() == HloOpcode::kReshape);
    const HloInstruction* store = reshape_2->users()[0];
    EXPECT_TRUE(IsPoplarInstruction(PoplarOp::RemoteParameterStore)(store));
  }
}

TEST_F(ResourceUpdateVariablesOffloadTest, ReadOnlyReplicated) {
  std::string hlo = R"(
HloModule top

resource_update {
  arg0 = f32[1,4,4,2] parameter(0)
  arg1 = f32[1,4,4,2] parameter(1)
  arg2 = f32[1,4,4,2] parameter(2)
  arg3 = f32[1,4,4,2] parameter(3)
  arg2_new = f32[1,4,4,2] add(arg2, arg0)
  arg3_new = f32[1,4,4,2] add(arg3, arg1)
  arg3_new_new = f32[1,4,4,2] add(arg3_new, arg2_new)
  ROOT t = (f32[1,4,4,2], f32[1,4,4,2], f32[1,4,4,2]) tuple(arg0, arg1, arg2, arg3_new_new)
}

loop {
  after-all = token[] after-all()
  infeed = (f32[1,4,4,2], token[]) infeed(after-all), infeed_config="140121807314576"
  in0 = f32[1,4,4,2] get-tuple-element(infeed), index=0
  in1 = f32[1,4,4,2] parameter(0)
  add1 = f32[1,4,4,2] add(in0, in1)
  in2 = f32[1,4,4,2] parameter(1)
  add2 = f32[1,4,4,2] add(in0, in2)
  in3 = f32[1,4,4,2] parameter(2)
  in4 = f32[1,4,4,2] parameter(3)
  call_ru = (f32[1,4,4,2], f32[1,4,4,2], f32[1,4,4,2], f32[1,4,4,2]) call(add1, add2, in3, in4), to_apply=resource_update, frontend_attributes={CALL_CONFIG_TYPE=ResourceUpdate}, backend_config="{\"callConfig\":{\"type\":\"ResourceUpdate\",\"resourceUpdateConfig\":{\"offloadVariables\":\"THREESTATE_ON\", \"partitionOffloadedVariables\":\"THREESTATE_ON\"}}}"
  gte0 = f32[1,4,4,2] get-tuple-element(call_ru), index=0
  gte1 = f32[1,4,4,2] get-tuple-element(call_ru), index=1
  gte2 = f32[1,4,4,2] get-tuple-element(call_ru), index=2
  gte3 = f32[1,4,4,2] get-tuple-element(call_ru), index=3
  ROOT tuple = (f32[1,4,4,2], f32[1,4,4,2], f32[1,4,4,2], f32[1,4,4,2]) tuple(gte0, gte1, gte2, gte3)
}

ENTRY e {
  e.in0 = f32[1,4,4,2] parameter(0), parameter_replication={false}
  e.in1 = f32[1,4,4,2] parameter(1), parameter_replication={false}
  e.in2 = f32[1,4,4,2] parameter(2), parameter_replication={false}
  e.in3 = f32[1,4,4,2] parameter(3), parameter_replication={false}
  e.call = (f32[1,4,4,2], f32[1,4,4,2], f32[1,4,4,2], f32[1,4,4,2]) call(e.in0, e.in1, e.in2, e.in3), to_apply=loop, backend_config="{\"callConfig\":{\"type\":\"RepeatLoop\",\"repeatConfig\":{\"repeatCount\":\"100\"}}}"
  gte0 = f32[1,4,4,2] get-tuple-element(e.call), index=0
  gte1 = f32[1,4,4,2] get-tuple-element(e.call), index=1
  gte2 = f32[1,4,4,2] get-tuple-element(e.call), index=2
  gte3 = f32[1,4,4,2] get-tuple-element(e.call), index=3
  ROOT t =  (f32[1,4,4,2], f32[1,4,4,2], f32[1,4,4,2], f32[1,4,4,2]) tuple(gte0, gte1, gte2, gte3)
}
)";
  auto config = GetModuleConfigForTest();
  config.set_resource_input_count(4);
  config.set_input_mapping({0, 1, 2, 3});
  config.set_resource_update_to_input_index({0, 1, 2, 3});
  TF_ASSERT_OK_AND_ASSIGN(auto module,
                          ParseAndReturnVerifiedModule(hlo, config));

  CompilerAnnotations annotations(module.get());
  ResourceUpdateVariablesOffload prvo(annotations, true, 0, 2);
  TF_ASSERT_OK_AND_ASSIGN(bool changed, prvo.Run(module.get()));
  EXPECT_TRUE(changed);

  auto root = module->entry_computation()->root_instruction();
  HloComputation* repeat_computation = root->operand(0)->operand(0)->to_apply();
  HloInstruction* repeat_root = repeat_computation->root_instruction();
  HloComputation* resource_update =
      repeat_root->mutable_operand(0)->mutable_operand(0)->to_apply();
  EXPECT_EQ(resource_update->num_parameters(), 4);
  EXPECT_EQ(ShapeUtil::TupleElementCount(
                resource_update->root_instruction()->shape()),
            4);

  // Check there is 1 store 2 loads, and 2 all-gathers.
  auto insts = resource_update->instructions();
  EXPECT_EQ(absl::c_count_if(
                insts, IsPoplarInstruction(PoplarOp::RemoteParameterStore)),
            1);
  EXPECT_EQ(absl::c_count_if(
                insts, IsPoplarInstruction(PoplarOp::RemoteParameterLoad)),
            2);
  EXPECT_EQ(absl::c_count_if(insts, IsPoplarInstruction(PoplarOp::AllGather)),
            2);

  HloInstruction* resource_update_root = resource_update->root_instruction();

  // Expect the first three inputs to be pass-through.
  for (int i : {0, 1, 2}) {
    const HloInstruction* operand = resource_update_root->operand(i);
    EXPECT_EQ(operand->opcode(), HloOpcode::kParameter);
  }

  // The final input should be updated with a store.
  const HloInstruction* final_operand = resource_update_root->operand(3);
  EXPECT_EQ(final_operand->opcode(), HloOpcode::kCustomCall);
  EXPECT_TRUE(
      IsPoplarInstruction(PoplarOp::RemoteParameterStore)(final_operand));
}

}  // namespace
}  // namespace poplarplugin
}  // namespace xla
