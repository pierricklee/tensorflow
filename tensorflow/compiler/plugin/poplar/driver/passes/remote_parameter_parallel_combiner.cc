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

#include "tensorflow/compiler/plugin/poplar/driver/passes/remote_parameter_parallel_combiner.h"

#include <algorithm>
#include <map>
#include <queue>
#include <utility>
#include <vector>

#include "tensorflow/compiler/plugin/poplar/driver/tools/custom_ops/remote_parameter.h"
#include "tensorflow/compiler/plugin/poplar/driver/tools/matcher_predicates.h"
#include "tensorflow/compiler/plugin/poplar/driver/tools/util.h"
#include "tensorflow/compiler/xla/service/hlo_casting_utils.h"
#include "tensorflow/compiler/xla/service/hlo_module.h"
#include "tensorflow/compiler/xla/service/hlo_reachability.h"
#include "tensorflow/compiler/xla/service/hlo_value.h"
#include "tensorflow/compiler/xla/shape_util.h"
#include "tensorflow/compiler/xla/status_macros.h"
#include "tensorflow/core/lib/core/errors.h"
#include "tensorflow/core/lib/core/status.h"
#include "tensorflow/core/lib/strings/str_util.h"

namespace xla {
namespace poplarplugin {

namespace {

bool IsRemoteLoad(const HloInstruction* inst) {
  return IsPoplarInstruction(RemoteParameterLoad)(inst) ||
         IsPoplarInstruction(BufferLoadSlice)(inst);
}

bool IsRemoteStore(const HloInstruction* inst) {
  return IsPoplarInstruction(RemoteParameterStore)(inst) ||
         IsPoplarInstruction(BufferStoreSlice)(inst);
}

std::vector<HloInstruction*> CombineOperands(
    const std::vector<HloInstruction*>& to_combine) {
  std::vector<HloInstruction*> operands;

  const auto* first_inst = to_combine.front();
  if (IsPoplarInstruction(RemoteParameterLoad)(first_inst)) {
    for (const auto* inst : to_combine) {
      operands.insert(operands.end(), inst->operands().cbegin(),
                      inst->operands().cend());
    }
  } else if (IsPoplarInstruction(BufferLoadSlice)(first_inst)) {
    std::vector<HloInstruction*> remote_buffers;
    std::vector<HloInstruction*> offsets;
    for (const auto* inst : to_combine) {
      const auto* load_inst = Cast<HloBufferLoadSlice>(inst);
      remote_buffers.insert(remote_buffers.end(),
                            load_inst->RemoteBuffers().cbegin(),
                            load_inst->RemoteBuffers().cend());
      offsets.insert(offsets.end(), load_inst->Offsets().cbegin(),
                     load_inst->Offsets().cend());
    }

    // The new list of operands has all the remote buffers first, then all the
    // corresponding offsets.
    operands.insert(operands.end(), remote_buffers.cbegin(),
                    remote_buffers.cend());
    operands.insert(operands.end(), offsets.cbegin(), offsets.cend());
  } else if (IsPoplarInstruction(RemoteParameterStore)(first_inst)) {
    std::vector<HloInstruction*> remote_buffers;
    std::vector<HloInstruction*> values_to_store;
    for (const auto* inst : to_combine) {
      const auto* store_inst = Cast<HloRemoteParameterStore>(inst);
      remote_buffers.insert(remote_buffers.end(),
                            store_inst->RemoteBuffers().cbegin(),
                            store_inst->RemoteBuffers().cend());
      values_to_store.insert(values_to_store.end(),
                             store_inst->ValuesToStore().cbegin(),
                             store_inst->ValuesToStore().cend());
    }

    // The new list of operands has all the remote buffers first, then all the
    // corresponding values to store.
    operands.insert(operands.end(), remote_buffers.cbegin(),
                    remote_buffers.cend());
    operands.insert(operands.end(), values_to_store.cbegin(),
                    values_to_store.cend());
  } else if (IsPoplarInstruction(BufferStoreSlice)(first_inst)) {
    std::vector<HloInstruction*> remote_buffers;
    std::vector<HloInstruction*> values_to_store;
    std::vector<HloInstruction*> offsets;
    for (const auto* inst : to_combine) {
      const auto* store_inst = Cast<HloBufferStoreSlice>(inst);
      remote_buffers.insert(remote_buffers.end(),
                            store_inst->RemoteBuffers().cbegin(),
                            store_inst->RemoteBuffers().cend());
      values_to_store.insert(values_to_store.end(),
                             store_inst->ValuesToStore().cbegin(),
                             store_inst->ValuesToStore().cend());
      offsets.insert(offsets.end(), store_inst->Offsets().cbegin(),
                     store_inst->Offsets().cend());
    }

    // The new list of operands has all the remote buffers first, then all the
    // corresponding values, and finally the corresponding offsets.
    operands.insert(operands.end(), remote_buffers.cbegin(),
                    remote_buffers.cend());
    operands.insert(operands.end(), values_to_store.cbegin(),
                    values_to_store.cend());
    operands.insert(operands.end(), offsets.cbegin(), offsets.cend());
  } else {
    LOG(FATAL) << "Unexpected instruction: " << first_inst->ToString();
  }

  return operands;
}

std::vector<uint64> CombineReplicationFactors(
    const std::vector<HloInstruction*>& to_combine) {
  std::vector<uint64> replication_factors(to_combine.size());
  absl::c_transform(
      to_combine, replication_factors.begin(), [](const HloInstruction* inst) {
        if (IsPoplarInstruction(RemoteParameterLoad)(inst)) {
          return Cast<HloRemoteParameterLoad>(inst)->GetReplicationFactor(0);
        } else if (IsPoplarInstruction(RemoteParameterStore)(inst)) {
          return Cast<HloRemoteParameterStore>(inst)->GetReplicationFactor(0);
        } else {
          LOG(FATAL) << "Unexpected instruction: " << inst->ToString();
        }
      });
  return replication_factors;
}

StatusOr<HloInstruction*> Combine(
    const std::vector<HloInstruction*>& to_combine, const Shape& shape) {
  const auto operands = CombineOperands(to_combine);
  auto* first_inst = to_combine.front();
  HloComputation* comp = first_inst->parent();

  if (IsPoplarInstruction(BufferLoadSlice)(first_inst) ||
      IsPoplarInstruction(BufferStoreSlice)(first_inst)) {
    return comp->AddInstruction(
        first_inst->CloneWithNewOperands(shape, operands));
  }

  const auto replication_factors = CombineReplicationFactors(to_combine);

  HloInstruction* new_inst = nullptr;
  if (IsPoplarInstruction(RemoteParameterLoad)(first_inst)) {
    new_inst = comp->AddInstruction(
        CreateHloRemoteParameterLoad(operands, replication_factors));
  } else if (IsPoplarInstruction(RemoteParameterStore)(first_inst)) {
    new_inst = comp->AddInstruction(
        CreateHloRemoteParameterStore(operands, replication_factors));
    CHECK(absl::c_all_of(to_combine, IsLoweredInplace));
  } else {
    return FailedPrecondition("Unexpected instruction: %s",
                              first_inst->ToString().c_str());
  }
  first_inst->SetupDerivedInstruction(new_inst);
  new_inst->set_raw_backend_config_string(
      first_inst->raw_backend_config_string());
  return new_inst;
}

StatusOr<HloInstruction*> CombineAndReplace(
    const std::vector<HloInstruction*>& to_combine) {
  CHECK_GE(to_combine.size(), 2);
  HloComputation* comp = to_combine.front()->parent();

  // Combine the shapes into a tuple.
  std::vector<Shape> shapes(to_combine.size());
  absl::c_transform(to_combine, shapes.begin(),
                    [](HloInstruction* inst) { return inst->shape(); });
  const auto shape = ShapeUtil::MakeTupleShape(shapes);

  // Add the new instruction.
  TF_ASSIGN_OR_RETURN(HloInstruction * new_inst, Combine(to_combine, shape));
  CHECK_EQ(new_inst->shape(), shape);

  // Combine the sharding information into a tuple.
  std::vector<HloSharding> shardings;
  for (const auto* inst : to_combine) {
    shardings.push_back(inst->sharding());
  }
  new_inst->set_sharding(HloSharding::Tuple(shape, shardings));

  for (std::size_t i = 0; i < to_combine.size(); ++i) {
    auto* inst = to_combine[i];

    // Add an in-place GTE to unpack the new_inst result.
    auto* gte = comp->AddInstruction(
        HloInstruction::CreateGetTupleElement(inst->shape(), new_inst, i));
    MakeUsedInplace(gte);
    gte->set_sharding(shardings[i]);

    // Replace the old inst.
    TF_RETURN_IF_ERROR(new_inst->CopyAllControlDepsFrom(inst));
    TF_RETURN_IF_ERROR(inst->DropAllControlDeps());
    TF_RETURN_IF_ERROR(inst->ReplaceAllUsesWith(gte));
    TF_RETURN_IF_ERROR(comp->RemoveInstruction(inst));
  }

  return new_inst;
}

bool IndependentlySchedulable(const std::vector<HloInstruction*>& instructions,
                              const HloReachabilityMap& reachability_map) {
  // Quadratic complexity in the number of shards; shouldn't be too bad.
  for (const auto* a : instructions) {
    for (const auto* b : instructions) {
      if (a != b && reachability_map.IsReachable(a, b)) {
        return false;
      }
    }
  }

  return true;
}

struct DecreasingSizeComparator {
  bool operator()(const HloInstruction* a, const HloInstruction* b) const {
    const auto a_size = ShapeUtil::ByteSizeOf(a->shape(), 1);
    const auto b_size = ShapeUtil::ByteSizeOf(b->shape(), 1);
    if (a_size != b_size) {
      return a_size < b_size;
    }

    // If the size is the same, order by parameter index.
    const int64 a_index = a->operand(0)->parameter_number();
    const int64 b_index = b->operand(0)->parameter_number();
    if (a_index != b_index) {
      return a_index > b_index;
    }

    // Everything else equal, defer to an arbitrary but deterministic order.
    return HloPtrComparator()(a, b);
  }
};

using DecreasingSizeQueue =
    std::priority_queue<HloInstruction*, std::vector<HloInstruction*>,
                        DecreasingSizeComparator>;

StatusOr<std::vector<HloInstruction*>> CombineFromDifferentShards(
    HloComputation* comp, std::map<int64, DecreasingSizeQueue> shard_queues) {
  std::vector<HloInstruction*> combined;

  while (true) {
    std::vector<HloInstruction*> to_combine;

    // Pop the largest one from each shard.
    for (auto& shard_queue : shard_queues) {
      auto& queue = shard_queue.second;
      if (!queue.empty()) {
        to_combine.push_back(queue.top());
        queue.pop();
      }
    }

    if (to_combine.size() < 2) {
      break;
    }

    // We must build a new reachability map because it does not support updates
    // to reflect the changes made by the combinations.
    const auto reachability_map = HloReachabilityMap::Build(comp);

    // We expect that the instructions in the different shards are not
    // dependent on each other, and hence can be combined safely. If this is
    // not the case, we just bail out of this attempt and try the next.
    if (!IndependentlySchedulable(to_combine, *reachability_map)) {
      VLOG(2) << "Skipping combination because of dependencies";
      continue;
    }

    TF_ASSIGN_OR_RETURN(auto* combined_inst, CombineAndReplace(to_combine));

    combined.push_back(combined_inst);
  }

  // Return the instructions in order from smallest to largest. When trying to
  // schedule them in this order later this can help liveness, since we load the
  // largest parameters last when other tensors (like gradients for already
  // updated weights) might not be alive anymore.
  absl::c_reverse(combined);

  return combined;
}

Status ScheduleAllUsersBefore(HloInstruction* inst, HloInstruction* successor,
                              HloReachabilityMap* reachability_map,
                              HloInstructionSet* scheduled_users) {
  for (auto* user : inst->users()) {
    if (!reachability_map->IsReachable(successor, user)) {
      if (scheduled_users->insert(user).second) {
        TF_RETURN_IF_ERROR(user->AddControlDependencyTo(successor));
        reachability_map->UpdateReachabilityThroughInstruction(successor);
        TF_RETURN_IF_ERROR(ScheduleAllUsersBefore(
            user, successor, reachability_map, scheduled_users));
      }
    }
  }

  return Status::OK();
}

Status ScheduleAllInputsAfter(HloInstruction* inst, HloInstruction* predecessor,
                              HloReachabilityMap* reachability_map,
                              HloInstructionSet* scheduled_inputs) {
  for (auto* operand : inst->unique_operands()) {
    if (operand->opcode() == HloOpcode::kParameter) {
      continue;
    }

    if (!reachability_map->IsReachable(operand, predecessor)) {
      if (scheduled_inputs->insert(operand).second) {
        TF_RETURN_IF_ERROR(predecessor->AddControlDependencyTo(operand));
        reachability_map->UpdateReachabilityThroughInstruction(operand);
        TF_RETURN_IF_ERROR(ScheduleAllInputsAfter(
            operand, predecessor, reachability_map, scheduled_inputs));
      }
    }
  }

  return Status::OK();
}

Status AddSchedulingConstraints(
    HloComputation* comp, const std::vector<HloInstruction*>& combined_loads,
    const std::vector<HloInstruction*>& combined_stores) {
  if (combined_loads.size() != combined_stores.size()) {
    // They're not matching up, bail out.
    return Status::OK();
  }

  auto reachability_map = HloReachabilityMap::Build(comp);

  for (std::size_t i = 1; i < combined_loads.size(); ++i) {
    auto* load = combined_loads[i];

    // To minimize liveness we aim towards having the least amount of overlap.
    // So first we try to schedule load[i] after store[i - 1], and if this is
    // not possible, we try to schedule it after store[i - 2] and so forth. A
    // typical reason why the first single-delay attempt might fail is when
    // using optimizers that require two offloaded parameters for each weight
    // update (like LAMB/ADAM that require both the first and second moments).
    for (std::size_t delay = 1; delay <= i; ++delay) {
      auto* prev_store = combined_stores[i - delay];

      // If we can successfully schedule the previous store before this load,
      // we are satisifed with the scheduling constraints for this load and
      // break out to the next one.
      if (!reachability_map->IsReachable(load, prev_store)) {
        TF_RETURN_IF_ERROR(prev_store->AddControlDependencyTo(load));
        reachability_map->UpdateReachabilityThroughInstruction(load);

        // To minimze liveness, we also try to schedule all users of the
        // previous load before the current load. This attempts to ensure that
        // the actual weight update is pushed as early as possible in the
        // schedule, as soon as the necessary remote parameters are loaded.
        auto* prev_load = combined_loads[i - delay];
        HloInstructionSet scheduled_users;
        TF_RETURN_IF_ERROR(ScheduleAllUsersBefore(
            prev_load, load, reachability_map.get(), &scheduled_users));

        // In addition, we try to schedule all the inputs needed by these users
        // after the previous load. This attempts to make sure that there is
        // as little overlap as possible between the updates of the different
        // weights. For example, some models will cast all the gradients from
        // float16 to float32, and this attempts to make sure that is done as
        // late as possible in the schedule.
        HloInstructionSet scheduled_inputs;
        for (auto* user : scheduled_users) {
          TF_RETURN_IF_ERROR(ScheduleAllInputsAfter(
              user, prev_load, reachability_map.get(), &scheduled_inputs));
        }

        break;
      }
    }
  }

  return Status::OK();
}

}  // namespace

StatusOr<bool> RemoteParameterParallelCombiner::RunOnComputation(
    HloComputation* comp) {
  std::map<int64, DecreasingSizeQueue> shard_loads;
  std::map<int64, DecreasingSizeQueue> shard_stores;

  for (auto* inst : comp->MakeInstructionPostOrder()) {
    if (auto shard = inst->sharding_unique_device()) {
      if (IsRemoteLoad(inst)) {
        shard_loads[*shard].push(inst);
      } else if (IsRemoteStore(inst)) {
        shard_stores[*shard].push(inst);
      }
    }
  }

  TF_ASSIGN_OR_RETURN(const auto combined_loads,
                      CombineFromDifferentShards(comp, std::move(shard_loads)));

  TF_ASSIGN_OR_RETURN(
      const auto combined_stores,
      CombineFromDifferentShards(comp, std::move(shard_stores)));

  // Try to help the scheduler a bit by adding some constraints.
  TF_RETURN_IF_ERROR(
      AddSchedulingConstraints(comp, combined_loads, combined_stores));

  return !combined_loads.empty() || !combined_stores.empty();
}

StatusOr<bool> RemoteParameterParallelCombiner::Run(HloModule* module) {
  VLOG(2) << "Before RemoteParameterParallelCombiner:";
  XLA_VLOG_LINES(2, module->ToString());

  bool changed = false;

  // Run it for all resource updates.
  for (auto* comp : module->MakeComputationPostOrder()) {
    if (IsPopOpsFusion(comp)) {
      continue;
    }

    for (auto* inst : comp->MakeInstructionPostOrder()) {
      if (IsResourceUpdate(inst)) {
        TF_ASSIGN_OR_RETURN(const bool computation_changed,
                            RunOnComputation(inst->to_apply()));
        changed |= computation_changed;
      }
    }
  }

  if (changed) {
    VLOG(2) << "After RemoteParameterParallelCombiner:";
    XLA_VLOG_LINES(2, module->ToString());
  } else {
    VLOG(2) << "No changes were made.";
  }
  return changed;
}

}  // namespace poplarplugin
}  // namespace xla
