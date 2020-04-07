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

#include "tensorflow/compiler/plugin/poplar/driver/passes/host_compute_barrier_inserter.h"

#include <string>
#include <unordered_map>
#include <vector>

#include "tensorflow/compiler/plugin/poplar/driver/tools/custom_ops/send_recv_barrier.h"
#include "tensorflow/compiler/plugin/poplar/driver/tools/matcher_predicates.h"
#include "tensorflow/compiler/plugin/poplar/driver/tools/util.h"
#include "tensorflow/compiler/xla/service/hlo_casting_utils.h"
#include "tensorflow/compiler/xla/service/hlo_instructions.h"
#include "tensorflow/compiler/xla/service/hlo_module.h"
#include "tensorflow/core/lib/core/errors.h"
#include "tensorflow/core/lib/core/status.h"

namespace xla {
namespace poplarplugin {

namespace {

static constexpr char kHostComputeOp[] = "XlaHostCompute";

struct SendRecvs {
  std::vector<HloInstruction*> sends;
  std::vector<HloInstruction*> recvs;
};

using OpSendRecvs = std::unordered_map<string, SendRecvs>;

OpSendRecvs GroupSendRecvsByHostComputeOp(const HloComputation* comp) {
  OpSendRecvs result;

  auto is_send = IsPoplarInstruction(SendToHost);
  auto is_recv = IsPoplarInstruction(RecvFromHost);

  for (HloInstruction* inst : comp->instructions()) {
    if (inst->metadata().op_type() == kHostComputeOp) {
      const auto& op_name = inst->metadata().op_name();
      if (is_send(inst)) {
        result[op_name].sends.push_back(inst);
      } else if (is_recv(inst)) {
        result[op_name].recvs.push_back(inst);
      }
    }
  }

  return result;
}

StatusOr<bool> AddBarrier(HloComputation* comp,
                          const OpSendRecvs& op_send_recvs) {
  bool added = false;

  for (const auto& op_send_recv : op_send_recvs) {
    const auto& op_name = op_send_recv.first;
    const auto& sends = op_send_recv.second.sends;
    const auto& recvs = op_send_recv.second.recvs;

    VLOG(2) << "Found " << sends.size() << " sends and " << recvs.size()
            << " recvs from the op " << op_name;

    if (sends.empty() || recvs.empty()) {
      // Barrier only needed when there are both sends and recvs
      continue;
    }

    auto* barrier = comp->AddInstruction(CreateSendRecvBarrier());
    barrier->SetAndSanitizeName(op_name + ".barrier");
    added = true;

    for (auto* send : sends) {
      // Send before barrier
      TF_RETURN_IF_ERROR(send->AddControlDependencyTo(barrier));
    }

    for (auto* recv : recvs) {
      // Recv after barrier
      TF_RETURN_IF_ERROR(barrier->AddControlDependencyTo(recv));
    }
  }

  return added;
}

}  // namespace

StatusOr<bool> HostComputeBarrierInserter::Run(HloModule* module) {
  bool changed = false;

  for (HloComputation* comp : module->computations()) {
    if (IsPopOpsFusion(comp)) {
      // Optimization: Popops fusion computations cannot have Send/Recv ops.
      continue;
    }

    const OpSendRecvs op_send_recvs = GroupSendRecvsByHostComputeOp(comp);
    TF_ASSIGN_OR_RETURN(const bool added, AddBarrier(comp, op_send_recvs));
    changed |= added;
  }

  return changed;
}

}  // namespace poplarplugin
}  // namespace xla
