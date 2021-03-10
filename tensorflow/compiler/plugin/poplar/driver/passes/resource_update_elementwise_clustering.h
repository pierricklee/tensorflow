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

#ifndef TENSORFLOW_COMPILER_PLUGIN_POPLAR_DRIVER_PASSES_RESOURCE_UPDATE_ELEMENTWISE_CLUSTERING_H_
#define TENSORFLOW_COMPILER_PLUGIN_POPLAR_DRIVER_PASSES_RESOURCE_UPDATE_ELEMENTWISE_CLUSTERING_H_

#include <string>
#include <vector>

#include "tensorflow/compiler/plugin/poplar/driver/backend_config.pb.h"
#include "tensorflow/compiler/xla/service/hlo_pass_interface.h"
#include "tensorflow/compiler/xla/statusor.h"

namespace xla {

class HloModule;

namespace poplarplugin {

struct UserPositions {
  HloInstruction* instruction;
  std::vector<int64> indices;

  std::string ToString() const {
    return absl::StrCat("UserPositions: ", instruction->name(), ":",
                        absl::StrJoin(indices, ","));
  }
};

using CrossReplicaValidInputs = absl::flat_hash_set<const HloInstruction*>;

class ElementwiseCluster {
 public:
  explicit ElementwiseCluster(HloInstruction* top) noexcept;
  bool In(HloInstruction* inst) const;
  bool AnyUserIn(HloInstruction* inst) const;
  bool AllUsersIn(HloInstruction* inst) const;
  void Add(HloInstruction* inst);
  bool MaybeAdd(HloInstruction* inst);
  bool CanMerge(const ElementwiseCluster& other);
  void Merge(const ElementwiseCluster& other);
  const HloInstruction* GetTop() const;
  HloComputation* GetComputation() const;
  std::string Dump() const;

  // Finalize the cluster - no more instructions will be added. Returns whether
  // this is a cluster which should be processed further.
  bool Finalize(const CrossReplicaValidInputs& cross_replica_valid_inputs,
                ThreeState partition_offload_variables,
                uint32 replication_factor);

  // Following functions can be called once finalized.
  std::string ToString() const;
  const std::vector<HloInstruction*>& GetInputs() const;
  const std::vector<HloInstruction*>& GetPostOrder() const;
  const std::vector<HloInstruction*>& GetOutputs() const;
  const std::vector<UserPositions>& GetUsersForOutput(
      HloInstruction* inst) const;

  // The dimensions of the operations in the cluster before it is partitioned.
  const std::vector<int64>& GetClusterDimensions() const;
  // The dimensions of the operations in the cluster after it is partitioned.
  const std::vector<int64>& GetShardDimensions() const;
  // The size of the cluster before it is partitioned.
  int64 GetClusterSize() const;
  // The size of the cluster taking the padding on all-gathers into account.
  int64 GetAlignedClusterSize() const;
  // The size of the partitioned shape.
  int64 GetShardSize() const;
  // Whether this cluster is replica partitioned.
  bool IsReplicaPartitioned() const;
  // Returns original shape of the top-level instruction.
  Shape GetClusterShape(PrimitiveType type) const;

 private:
  HloInstruction* top_;
  Shape cluster_shape_;
  HloInstructionSet insts_;
  HloInstructionSet inputs_;
  bool finalized_ = false;

  // Populated once finalized.
  bool is_replica_partitioned_;
  std::vector<HloInstruction*> inputs_vec_;
  std::vector<HloInstruction*> post_order_;
  std::vector<HloInstruction*> outputs_;
  HloInstructionMap<std::vector<UserPositions>> outputs_to_users_;
  std::vector<int64> cluster_dimensions_;
  std::vector<int64> shard_dimensions_;
  int64 cluster_size_;
  int64 shard_size_;
  int64 aligned_cluster_size_;
};

// Find and replace clusters of elementwise instructions, sharding resource
// update computation across replicas. For each cluster, remove
// all-gather(remote-parameter-load) and store result in remote buffer shard.
class ResourceUpdateElementwiseClustering : public HloModulePass {
 public:
  explicit ResourceUpdateElementwiseClustering(
      uint32 replication_factor, bool handle_non_replicated_clusters = false)
      : replication_factor_(replication_factor),
        handle_non_replicated_clusters_(handle_non_replicated_clusters) {}

  absl::string_view name() const override {
    return "resource-update-elementwise-clustering";
  }

  StatusOr<bool> Run(HloModule* module);

  // Returns all computations in the module which are elementwise and can be
  // clustered.
  static absl::flat_hash_set<const HloComputation*>
  GetElementwiseClusterableComputations(const HloModule* module);

  // Get clusters inside of the call, where the call has to be a repeat loop or
  // a pipeline.
  static StatusOr<std::vector<ElementwiseCluster>> GetClustersIn(
      HloInstruction* const call,
      const absl::flat_hash_set<const HloComputation*>& elementwise_comps,
      uint32 replication_factor);

  // Outline the provided cluster - returns the call instruction to the cluster.
  static StatusOr<HloInstruction*> OutlineCluster(ElementwiseCluster& cluster,
                                                  uint32 replication_factor);

 private:
  uint32 replication_factor_;
  bool handle_non_replicated_clusters_;
};

}  // namespace poplarplugin
}  // namespace xla

#endif  // TENSORFLOW_COMPILER_PLUGIN_POPLAR_DRIVER_PASSES_RESOURCE_UPDATE_ELEMENTWISE_CLUSTERING_H_
