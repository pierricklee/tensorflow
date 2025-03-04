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

#include <cstring>
#include <fstream>
#include <utility>
#include <vector>

#include "tensorflow/compiler/plugin/poplar/driver/tools/poplar_executable_binary_file.h"
#include "tensorflow/compiler/plugin/poplar/driver/tools/poplar_util.h"

namespace xla {
namespace poplarplugin {

const std::array<uint8, 8> MAGIC_STRING{0x6e, 0x76, 0x64, 0x61,
                                        0xf0, 0x9f, 0x96, 0x95};

Status PoplarExecutableBinaryFile::Write(
    const std::string& file_name,
    const ::tensorflow::protobuf::MessageLite& proto,
    std::function<void(std::ostream&)> serialize_executable) {
  auto file = std::ofstream(file_name, std::ios::binary);
  if (!file) {
    return InternalErrorStrCat("Failed to open file for writing: ", file_name);
  }

  file.write(reinterpret_cast<const char*>(MAGIC_STRING.data()),
             MAGIC_STRING.size());

  std::string serialized;
  proto.AppendToString(&serialized);

  // Write the length of the serialized protobuf metadata - make sure to store
  // it as an 8 byte value and store it in a endian-independent format.
  std::array<uint8, 8> proto_length_bytes;
  for (uint64 i = 0; i != proto_length_bytes.size(); ++i) {
    proto_length_bytes[i] = (serialized.size() >> i * 8ULL) & 0xFFU;
  }

  file.write(reinterpret_cast<char*>(proto_length_bytes.data()),
             proto_length_bytes.size());

  // Append the protobuf metadata.
  file << serialized;

  // Append the Poplar executable.
  try {
    serialize_executable(file);
  } catch (const std::exception& e) {
    return PoplarExceptionToTensorflowStatus("[Serialize]", e);
  }

  return Status::OK();
}

StatusOr<poplar::Executable> PoplarExecutableBinaryFile::Read(
    const std::string& file_name, ::tensorflow::protobuf::MessageLite* proto) {
  auto file = absl::make_unique<std::ifstream>(file_name, std::ios::binary);
  const std::string error_prefix =
      absl::StrCat("[Deserialize][File: ", file_name, "]");

  std::array<uint8, MAGIC_STRING.size()> magic_string;
  if (!file->read(reinterpret_cast<char*>(magic_string.data()),
                  magic_string.size())) {
    return InternalErrorStrCat(
        error_prefix, "Corrupted - Cannot read the executable magic number.");
  }

  if (memcmp(magic_string.data(), MAGIC_STRING.data(), MAGIC_STRING.size()) !=
      0) {
    return InternalErrorStrCat(
        error_prefix,
        "Corrupted - Magic string does not contain expected value.");
  }

  // Read the protobuf metadata length.
  std::array<uint8, 8> proto_length_bytes;
  if (!file->read(reinterpret_cast<char*>(proto_length_bytes.data()),
                  proto_length_bytes.size())) {
    return InternalErrorStrCat(error_prefix,
                               " Corrupted - Cannot read the metadata length.");
  }
  uint64 metadata_length = 0;
  for (uint64 i = 0; i != proto_length_bytes.size(); ++i) {
    metadata_length |= static_cast<uint64>(proto_length_bytes[i]) << i * 8ULL;
  }

  // Read the protobuf metadata.
  std::vector<char> serialized(metadata_length);
  if (!file->read(serialized.data(), metadata_length)) {
    return InternalErrorStrCat(error_prefix,
                               " Corrupted - Cannot read the metadata.");
  }
  if (!proto->ParseFromArray(serialized.data(), metadata_length)) {
    return InternalErrorStrCat(error_prefix,
                               " Corrupted - Cannot parse the metadata.");
  }
  serialized.clear();

  // Read the executable.
  try {
    return poplar::Executable::deserialize(std::move(file));
  } catch (const std::exception& e) {
    return PoplarExceptionToTensorflowStatus(error_prefix, e);
  }
}

}  // namespace poplarplugin
}  // namespace xla
