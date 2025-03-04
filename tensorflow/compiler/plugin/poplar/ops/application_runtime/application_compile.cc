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

#include "tensorflow/core/framework/op.h"

namespace tensorflow {

REGISTER_OP("IPUApplicationCompile")
    .Input("args: Targs")
    .Attr("Targs: list(type) >= 0")
    .Attr("resource_indices: list(int) >= 0")
    .Attr("constant_indices: list(int) >= 0")
    .Attr("executable_output_path: string")
    .Output("output: string")
    .Attr("function: func")
    // Compilation cache is stateful.
    .SetIsStateful();

}  // namespace tensorflow
