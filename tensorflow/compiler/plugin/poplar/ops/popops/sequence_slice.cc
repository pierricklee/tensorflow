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

#include <numeric>
#include <vector>

#include "tensorflow/core/framework/common_shape_fns.h"
#include "tensorflow/core/framework/op.h"

namespace tensorflow {

REGISTER_OP("IpuSequenceSlice")
    .Input("dst: dtype")
    .Input("src: dtype")
    .Input("num_elems: int32")
    .Input("src_offsets: int32")
    .Input("dst_offsets: int32")
    .Output("output: dtype")
    .Attr("dtype: {float16, float32, int32}")
    .Attr("zero_unused: bool = false")
    .SetShapeFn([](shape_inference::InferenceContext* c) {
      c->set_output(0, c->input(0));
      return Status::OK();
    })
    .Doc(R"doc(
Internal implementation of SequenceSlice for sequence unpacking.
)doc");

// Special case of SequenceSlice.
REGISTER_OP("IpuSequenceSliceUnpack")
    .Input("src: dtype")
    .Input("num_elems: int32")
    .Input("src_offsets: int32")
    .Input("dst_offsets: int32")
    .Output("output: dtype")
    .Attr("total_elements: int")
    .Attr("dtype: {float16, float32, int32}")
    .SetShapeFn([](shape_inference::InferenceContext* c) {
      // Get the total number of rows to be sliced.
      int64 total_elements;
      c->GetAttr("total_elements", &total_elements);

      // Output shape is input shape with outermost dimension
      // replaced by the total number of slices.
      auto in_shape = c->input(0);
      decltype(in_shape) out_shape;
      c->ReplaceDim(in_shape, 0, c->MakeDim(total_elements), &out_shape);
      c->set_output(0, out_shape);
      return Status::OK();
    })
    .Doc(R"doc(
Internal implementation of SequenceSlice for sequence unpacking.
)doc");

}  // namespace tensorflow
