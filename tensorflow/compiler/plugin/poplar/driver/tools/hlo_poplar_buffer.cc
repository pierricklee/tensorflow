/* Copyright 2021 The TensorFlow Authors. All Rights Reserved.

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

#include "tensorflow/compiler/plugin/poplar/driver/tools/hlo_poplar_buffer.h"

#include <algorithm>
#include <utility>

#include "absl/container/flat_hash_set.h"
#include "absl/memory/memory.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"

#include "tensorflow/compiler/plugin/poplar/driver/tools/alias_info.pb.h"
#include "tensorflow/compiler/xla/service/hlo_instruction.h"
#include "tensorflow/compiler/xla/shape_tree.h"
#include "tensorflow/compiler/xla/shape_util.h"

namespace xla {
namespace poplarplugin {

const Shape& HloPoplarPosition::shape() const {
  return ShapeUtil::GetSubshape(instruction->shape(), index);
}

std::string HloPoplarPosition::ToString() const {
  return absl::StrCat(instruction->name(), index.ToString());
}

bool HloPoplarPosition::operator==(const HloPoplarPosition& other) const {
  return instruction == other.instruction && index == other.index;
}

bool HloPoplarPosition::operator!=(const HloPoplarPosition& other) const {
  return !(*this == other);
}

bool HloPoplarPosition::operator<(const HloPoplarPosition& other) const {
  // Stable less-than operator using instruction id and index.
  return instruction->unique_id() < other.instruction->unique_id() ||
         (instruction->unique_id() == other.instruction->unique_id() &&
          index < other.index);
}

std::ostream& operator<<(std::ostream& out, const HloPoplarPosition& position) {
  out << position.ToString();
  return out;
}

HloPoplarUseDescription::HloPoplarUseDescription(
    int64 operand_number, const ShapeIndex& operand_index,
    const ShapeIndex& output_index, BufferUseKind kind)
    : operand_number_(operand_number),
      operand_index_(operand_index),
      output_index_(output_index),
      kind_(kind) {}

PoplarUseDescription HloPoplarUseDescription::ToProto() const {
  PoplarUseDescription proto;
  proto.set_operand_number(operand_number());
  *(proto.mutable_operand_index()) = {operand_index().begin(),
                                      operand_index().end()};
  *(proto.mutable_output_index()) = {output_index().begin(),
                                     output_index().end()};
  proto.set_kind(kind());
  return proto;
}

/*static*/ HloPoplarUseDescription HloPoplarUseDescription::FromProto(
    const PoplarUseDescription& proto) {
  const ShapeIndex operand_index{proto.operand_index().begin(),
                                 proto.operand_index().end()};
  const ShapeIndex output_index{proto.output_index().begin(),
                                proto.output_index().end()};
  return HloPoplarUseDescription{proto.operand_number(), operand_index,
                                 output_index, proto.kind()};
}

std::string HloPoplarUseDescription::ToString() const {
  return absl::StrCat("alias ", BufferUseKind_Name(kind()), " from operand ",
                      operand_number(), " index ", operand_index().ToString(),
                      " to output index ", output_index().ToString());
}

bool HloPoplarUseDescription::operator==(
    const HloPoplarUseDescription& other) const {
  return std::make_tuple(operand_number(), operand_index(), output_index(),
                         kind()) ==
         std::make_tuple(other.operand_number(), other.operand_index(),
                         other.output_index(), other.kind());
}

bool HloPoplarUseDescription::operator!=(
    const HloPoplarUseDescription& other) const {
  return !(*this == other);
}

HloPoplarUse::HloPoplarUse(HloInstruction* instruction, int64 operand_number,
                           const ShapeIndex& operand_index, BufferUseKind kind)
    : instruction_(instruction),
      operand_number_(operand_number),
      operand_index_(operand_index),
      kind_(kind) {}

std::ostream& operator<<(std::ostream& out, const HloPoplarUse& use) {
  out << use.ToString();
  return out;
}

HloPoplarNoAliasUse::HloPoplarNoAliasUse(HloInstruction* instruction,
                                         int64 operand_number,
                                         const ShapeIndex& operand_index)
    : HloPoplarUse(instruction, operand_number, operand_index,
                   BufferUseKind::USE_NO_ALIAS) {}

std::string HloPoplarNoAliasUse::ToString() const {
  const std::string operand_index_str =
      instruction()->operand(operand_number())->shape().IsTuple()
          ? (" " + operand_index().ToString())
          : "";
  return absl::StrCat(instruction()->name(), ", alias-kind ",
                      BufferUseKind_Name(kind()), ", operand ",
                      operand_number(), operand_index_str);
}

HloPoplarAliasUseBase::HloPoplarAliasUseBase(
    HloInstruction* instruction, int64 operand_number,
    const ShapeIndex& operand_index,
    const std::vector<ShapeIndex> output_indices, BufferUseKind kind)
    : HloPoplarUse(instruction, operand_number, operand_index, kind),
      output_indices_(output_indices) {
  CHECK_GT(output_indices_.size(), 0);
}

std::string HloPoplarAliasUseBase::ToString() const {
  const std::string operand_index_str =
      instruction()->operand(operand_number())->shape().IsTuple()
          ? (" " + operand_index().ToString())
          : "";

  std::string output_indices_str = absl::StrJoin(
      output_indices(), ", ", [](std::string* result, const ShapeIndex& index) {
        result->append(index.ToString());
      });

  return absl::StrCat(instruction()->name(), ", alias-kind ",
                      BufferUseKind_Name(kind()), ", operand ",
                      operand_number(), operand_index_str, ", output ",
                      output_indices_str);
}

HloPoplarAliasReadOnlyUse::HloPoplarAliasReadOnlyUse(
    HloInstruction* instruction, int64 operand_number,
    const ShapeIndex& operand_index,
    const std::vector<ShapeIndex> output_indices)
    : HloPoplarAliasUseBase(instruction, operand_number, operand_index,
                            output_indices,
                            BufferUseKind::USE_ALIAS_READ_ONLY) {}

HloPoplarAliasReadWriteUse::HloPoplarAliasReadWriteUse(
    HloInstruction* instruction, int64 operand_number,
    const ShapeIndex& operand_index,
    const std::vector<ShapeIndex> output_indices)
    : HloPoplarAliasUseBase(instruction, operand_number, operand_index,
                            output_indices,
                            BufferUseKind::USE_ALIAS_READ_WRITE) {}

namespace {
std::string BufferLocalityToString(BufferLocality locality) {
  switch (locality) {
    case BufferLocality::kDeviceMemory: {
      return "DeviceMemory";
    }
    case BufferLocality::kRemoteMemory: {
      return "RemoteMemory";
    }
    default: {
      LOG(FATAL) << "Unknown BufferLocality";
      return "";
    }
  }
}
}  // namespace

HloPoplarBufferDescription::HloPoplarBufferDescription(
    const ShapeIndex& output_index, BufferLocality locality)
    : output_index_(output_index), locality_(locality) {}

std::string HloPoplarBufferDescription::ToString() const {
  return absl::StrCat("output index ", output_index().ToString(), " locality ",
                      BufferLocalityToString(locality()));
}

bool HloPoplarBufferDescription::operator==(
    const HloPoplarBufferDescription& other) const {
  return std::make_tuple(output_index(), locality()) ==
         std::make_tuple(other.output_index(), other.locality());
}

bool HloPoplarBufferDescription::operator!=(
    const HloPoplarBufferDescription& other) const {
  return !(*this == other);
}

HloPoplarBuffer::HloPoplarBuffer(HloPoplarBuffer::Id id,
                                 const HloPoplarPosition& defining_position,
                                 BufferLocality locality)
    : id_(id), defining_position_(defining_position), locality_(locality) {}

bool HloPoplarBuffer::operator==(const HloPoplarBuffer& other) const {
  const bool equal = defining_position() == other.defining_position();
  if (equal) {
    CHECK(locality() == other.locality());
    CHECK_EQ(id(), other.id());
  }
  return equal;
}

bool HloPoplarBuffer::operator!=(const HloPoplarBuffer& other) const {
  return !(*this == other);
}

string HloPoplarBuffer::ToString() const {
  return absl::StrCat("Id ", id_, " ", defining_position().ToString(),
                      ", locality ", BufferLocalityToString(locality()));
}

std::ostream& operator<<(std::ostream& out, const HloPoplarBuffer& buffer) {
  out << buffer.ToString();
  return out;
}

HloPoplarBufferSet::HloPoplarBufferSet(
    absl::Span<const HloPoplarBuffer* const> buffers)
    : buffers_(buffers.begin(), buffers.end()) {
  SortAndUniquifyBuffers();
}

const HloPoplarBuffer& HloPoplarBufferSet::GetUniqueBuffer() const {
  CHECK_EQ(buffers_.size(), 1);
  return *buffers_[0];
}

bool HloPoplarBufferSet::AddBuffer(const HloPoplarBuffer* buffer) {
  // Find the position where to insert it.
  auto it = std::lower_bound(buffers_.begin(), buffers_.end(), buffer,
                             HloPoplarBuffer::IdLessThan);
  if (it == buffers_.end() || (*it)->id() != buffer->id()) {
    buffers_.insert(it, buffer);
    return true;
  }
  return false;
}

bool HloPoplarBufferSet::operator==(const HloPoplarBufferSet& other) const {
  if (buffers_.size() != other.buffers_.size()) {
    return false;
  }

  for (size_t i = 0; i != buffers_.size(); ++i) {
    if (buffers_[i]->id() != other.buffers_[i]->id()) {
      return false;
    }
  }
  return true;
}

bool HloPoplarBufferSet::operator!=(const HloPoplarBufferSet& other) const {
  return !(*this == other);
}

void HloPoplarBufferSet::SortAndUniquifyBuffers() {
  absl::c_sort(buffers_, HloPoplarBuffer::IdLessThan);
  buffers_.erase(
      std::unique(buffers_.begin(), buffers_.end(), HloPoplarBuffer::IdEqual),
      buffers_.end());
}

std::string HloPoplarBufferSet::ToString() const {
  return absl::StrCat(
      "HloPoplarBufferSet: ",
      absl::StrJoin(buffers_, ", ",
                    [](std::string* result, const HloPoplarBuffer* buffer) {
                      result->append(buffer->ToString());
                    }));
}

std::ostream& operator<<(std::ostream& out,
                         const HloPoplarBufferSet& buffer_set) {
  out << buffer_set.ToString();
  return out;
}

InstructionPoplarBufferSet::InstructionPoplarBufferSet(const Shape& shape)
    : shape_(shape), buffer_sets_(shape) {}

void InstructionPoplarBufferSet::SetOutputBufferSet(
    const ShapeIndex& output_index, const HloPoplarBufferSet& buffer_set) {
  ShapeIndexView index_view(output_index);
  CHECK(buffer_sets_.IsLeaf(index_view));
  *buffer_sets_.mutable_element(index_view) = buffer_set;
}

const HloPoplarBufferSet& InstructionPoplarBufferSet::GetOutputBufferSet(
    const ShapeIndex& output_index) {
  ShapeIndexView index_view(output_index);
  CHECK(buffer_sets_.IsLeaf(index_view));
  return buffer_sets_.element(index_view);
}

std::string InstructionPoplarBufferSet::ToString() const {
  std::string out = absl::StrCat("InstructionPoplarBufferSet(",
                                 ShapeUtil::HumanString(shape_), ")\n");
  for (auto leaf : buffer_sets_.leaves()) {
    absl::StrAppend(&out, "  ", leaf.first.ToString(), " : ",
                    leaf.second.ToString(), "\n");
  }
  return out;
}

std::ostream& operator<<(
    std::ostream& out,
    const InstructionPoplarBufferSet& instruction_buffer_set) {
  out << instruction_buffer_set.ToString();
  return out;
}

}  // namespace poplarplugin
}  // namespace xla
