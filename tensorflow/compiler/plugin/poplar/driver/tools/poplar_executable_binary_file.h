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

#ifndef TENSORFLOW_COMPILER_PLUGIN_POPLAR_DRIVER_TOOLS_POPLAR_EXECUTABLE_BINARY_FILE_H_
#define TENSORFLOW_COMPILER_PLUGIN_POPLAR_DRIVER_TOOLS_POPLAR_EXECUTABLE_BINARY_FILE_H_

#include <array>
#include <functional>
#include <string>

#include <poplar/Executable.hpp>

#include "tensorflow/compiler/xla/service/executable.h"
#include "tensorflow/compiler/xla/statusor.h"

namespace xla {
namespace poplarplugin {

class PoplarExecutableBinaryFile {
 public:
  static Status Write(const std::string& file_name,
                      const ::tensorflow::protobuf::MessageLite& proto,
                      std::function<void(std::ostream&)> serialize_executable);

  static StatusOr<poplar::Executable> Read(
      const std::string& file_name, ::tensorflow::protobuf::MessageLite* proto);
};

}  // namespace poplarplugin
}  // namespace xla

#endif  // TENSORFLOW_COMPILER_PLUGIN_POPLAR_DRIVER_TOOLS_POPLAR_EXECUTABLE_BINARY_FILE_H_
