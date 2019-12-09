/* Copyright 2019 The TensorFlow Authors. All Rights Reserved.

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

#include <thread>

#include "absl/strings/str_cat.h"
#include "tensorflow/compiler/plugin/poplar/driver/poplar_executor.h"
#include "tensorflow/compiler/plugin/poplar/driver/poplar_platform.h"
#include "tensorflow/compiler/plugin/poplar/driver/tensor.h"
#include "tensorflow/compiler/tf2xla/shape_util.h"
#include "tensorflow/compiler/tf2xla/type_util.h"
#include "tensorflow/compiler/tf2xla/xla_op_kernel.h"

namespace tensorflow {

namespace {

xla::StatusOr<xla::poplarplugin::PoplarExecutor*> GetPoplarExecutor(
    int device_ordinal) {
  TF_ASSIGN_OR_RETURN(auto* platform,
                      se::MultiPlatformManager::PlatformWithName("Poplar"));

  auto* poplar_platform =
      static_cast<xla::poplarplugin::PoplarPlatform*>(platform);

  TF_ASSIGN_OR_RETURN(auto* stream_executor,
                      poplar_platform->ExecutorForDevice(device_ordinal));

  return static_cast<xla::poplarplugin::PoplarExecutor*>(
      stream_executor->implementation());
}

string CreateRendezvousKey(const string& key, const string& send_device,
                           const string& recv_device) {
  const uint64 send_device_incarnation = 0;
  return Rendezvous::CreateKey(send_device, send_device_incarnation,
                               recv_device, key, FrameAndIter{0, 0});
}

// TODO(hakons): These device strings do not need to match the actual
// devices, they only have to be unique for each stream. The unique
// "key" for each host computation should make sure this is the case.
// Should maybe make this more clear somehow.

string CreateSendRendezvousKey(const string& key, int index) {
  return CreateRendezvousKey(strings::StrCat(key, ":", index), "/device:IPU:0",
                             "/device:CPU:0");
}

string CreateRecvRendezvousKey(const string& key, int index) {
  return CreateRendezvousKey(strings::StrCat(key, ":", index), "/device:CPU:0",
                             "/device:IPU:0");
}

}  // namespace

class XlaHostComputeOp : public XlaOpKernel {
 public:
  explicit XlaHostComputeOp(OpKernelConstruction* ctx) : XlaOpKernel(ctx) {
    OP_REQUIRES(
        ctx, ctx->num_inputs() > 0 || ctx->num_outputs() > 0,
        errors::InvalidArgument(
            "Outside compilation scope must have either input or output"));

    std::vector<TensorShape> shapes;
    std::vector<tensorflow::DataType> types;
    OP_REQUIRES_OK(ctx, ctx->GetAttr("shapes", &shapes));
    OP_REQUIRES_OK(ctx, ctx->GetAttr("Toutputs", &types));

    OP_REQUIRES(
        ctx, shapes.size() == ctx->num_outputs(),
        errors::InvalidArgument("All output shapes from outside compilation "
                                "scope must be statically known"));

    OP_REQUIRES(
        ctx, shapes.size() == types.size(),
        errors::InvalidArgument("Must have same number of shapes and types"));

    for (size_t i = 0; i < shapes.size(); ++i) {
      xla::PrimitiveType xla_type;
      OP_REQUIRES_OK(ctx, DataTypeToPrimitiveType(types[i], &xla_type));
      recv_shapes_.push_back(TensorShapeToXLAShape(xla_type, shapes[i]));
    }

    OP_REQUIRES_OK(ctx, ctx->GetAttr("key", &key_));
  }

  void Compile(XlaOpKernelContext* ctx) override {
    for (int i = 0; i < ctx->num_inputs(); ++i) {
      BuildInput(ctx, i);
    }

    for (int i = 0; i < ctx->num_outputs(); ++i) {
      BuildOutput(ctx, i);
    }
  }

 private:
  void BuildInput(XlaOpKernelContext* ctx, int index) {
    xla::XlaBuilder* builder = ctx->builder();
    XlaCompiler* compiler = ctx->compiler();

    const xla::XlaOp send_token = CreateToken(builder);
    const xla::XlaOp input = ctx->Input(index);
    const TensorShape input_shape = ctx->InputShape(index);

    const DataType dtype = ctx->input_type(index);
    xla::Shape xla_shape;
    OP_REQUIRES_OK(ctx, TensorShapeToXLAShape(dtype, input_shape, &xla_shape));

    const auto rendezvous_key = CreateSendRendezvousKey(key_, index);

    xla::ChannelHandle send_channel;
    OP_REQUIRES_OK(ctx, compiler->GetDeviceToHostChannelHandle(rendezvous_key,
                                                               &send_channel));

    const xla::XlaOp send_done =
        xla::SendToHost(input, send_token, xla_shape, send_channel);

    OP_REQUIRES_OK(ctx, builder->SetInstructionFrontendAttribute(
                            send_done, "rendezvous_key", rendezvous_key));
  }

  void BuildOutput(XlaOpKernelContext* ctx, int index) {
    xla::XlaBuilder* builder = ctx->builder();
    XlaCompiler* compiler = ctx->compiler();

    const auto rendezvous_key = CreateRecvRendezvousKey(key_, index);

    xla::ChannelHandle recv_channel;
    OP_REQUIRES_OK(ctx, compiler->GetHostToDeviceChannelHandle(rendezvous_key,
                                                               &recv_channel));

    const xla::XlaOp recv_token = CreateToken(builder);
    const xla::XlaOp recv_done =
        xla::RecvFromHost(recv_token, recv_shapes_[index], recv_channel);

    OP_REQUIRES_OK(ctx, builder->SetInstructionFrontendAttribute(
                            recv_done, "rendezvous_key", rendezvous_key));

    const xla::XlaOp output = xla::GetTupleElement(recv_done, 0);
    ctx->SetOutput(index, output);
  }

  string key_;
  std::vector<xla::Shape> recv_shapes_;

  TF_DISALLOW_COPY_AND_ASSIGN(XlaHostComputeOp);
};

class XlaRecvAtHostOp : public AsyncOpKernel {
 public:
  explicit XlaRecvAtHostOp(OpKernelConstruction* ctx) : AsyncOpKernel(ctx) {
    OP_REQUIRES_OK(ctx, ctx->GetAttr("device_ordinal", &device_ordinal_));

    OP_REQUIRES(ctx, device_ordinal_ >= 0,
                errors::InvalidArgument("Need device_ordinal >= 0, got ",
                                        device_ordinal_));

    string key;
    OP_REQUIRES_OK(ctx, ctx->GetAttr("key", &key));

    for (int i = 0; i < ctx->num_outputs(); ++i) {
      const string full_key = CreateSendRendezvousKey(key, i);
      Rendezvous::ParsedKey parsed_key;
      OP_REQUIRES_OK(ctx, Rendezvous::ParseKey(full_key, &parsed_key));
      parsed_keys_.push_back(std::move(parsed_key));
    }
  }

  void ComputeAsync(OpKernelContext* ctx, DoneCallback done) override {
    auto poplar_executor = GetPoplarExecutor(device_ordinal_);
    OP_REQUIRES_OK_ASYNC(ctx, poplar_executor.status(), done);
    auto* rendezvous = poplar_executor.ValueOrDie()->GetRendezvous();

    output_accumulator_.emplace(ctx, std::move(done));

    for (int i = 0; i < ctx->num_outputs(); ++i) {
      rendezvous->RecvAsync(
          parsed_keys_[i], Rendezvous::Args{},
          [this, i](const Status& status, const Rendezvous::Args& send_args,
                    const Rendezvous::Args& recv_args, const Tensor& val,
                    bool is_dead) {
            CHECK(!is_dead);
            output_accumulator_->SetOutput(i, status, val);
          });
    }
  }

  // Accumulate the outputs asynchronously and call the done callback
  // when all of them have been received (and forwarded). Currently
  // it assumes all SetOutput() calls come from the same thread.
  class OutputAccumulator {
   public:
    OutputAccumulator(OpKernelContext* ctx, DoneCallback&& done)
        : ctx_(ctx),
          done_(std::move(done)),
          num_outputs_remaining_(ctx->num_outputs()),
          thread_id_() {}

    void SetOutput(int index, const Status& status, const Tensor& val) {
      OP_REQUIRES_OK_ASYNC(ctx_, status, done_);
      CheckThreadId();

      ctx_->set_output(index, val);

      CHECK_GT(num_outputs_remaining_, 0);
      --num_outputs_remaining_;
      if (num_outputs_remaining_ == 0) {
        done_();
      }
    }

   private:
    void CheckThreadId() {
      // The current Poplar will invoke all the stream callbacks from
      // the same thread. We embrace this here by e.g. not using an
      // atomic counter. So let's make sure this assumption still holds.
      if (thread_id_ == std::thread::id()) {
        thread_id_ = std::this_thread::get_id();
      } else {
        CHECK_EQ(thread_id_, std::this_thread::get_id())
            << " All usage must be from the same thread";
      }
    }

    OpKernelContext* ctx_;
    DoneCallback done_;
    int num_outputs_remaining_;
    std::thread::id thread_id_;
  };

 private:
  int device_ordinal_;
  std::vector<Rendezvous::ParsedKey> parsed_keys_;
  absl::optional<OutputAccumulator> output_accumulator_;

  TF_DISALLOW_COPY_AND_ASSIGN(XlaRecvAtHostOp);
};

class XlaSendFromHostOp : public OpKernel {
 public:
  explicit XlaSendFromHostOp(OpKernelConstruction* ctx) : OpKernel(ctx) {
    OP_REQUIRES_OK(ctx, ctx->GetAttr("device_ordinal", &device_ordinal_));

    OP_REQUIRES(ctx, device_ordinal_ >= 0,
                errors::InvalidArgument("Need device_ordinal >= 0, got ",
                                        device_ordinal_));

    string key;
    OP_REQUIRES_OK(ctx, ctx->GetAttr("key", &key));

    for (int i = 0; i < ctx->num_inputs(); ++i) {
      const string full_key = CreateRecvRendezvousKey(key, i);
      Rendezvous::ParsedKey parsed_key;
      OP_REQUIRES_OK(ctx, Rendezvous::ParseKey(full_key, &parsed_key));
      parsed_keys_.push_back(std::move(parsed_key));
    }
  }

  void Compute(OpKernelContext* ctx) override {
    auto poplar_executor = GetPoplarExecutor(device_ordinal_);
    OP_REQUIRES_OK(ctx, poplar_executor.status());
    auto* rendezvous = poplar_executor.ValueOrDie()->GetRendezvous();

    for (int i = 0; i < ctx->num_inputs(); ++i) {
      const Tensor& tensor = ctx->input(i);
      rendezvous->Send(parsed_keys_[i], Rendezvous::Args{}, tensor,
                       /*is_dead=*/false);
    }
  }

 private:
  int device_ordinal_;
  std::vector<Rendezvous::ParsedKey> parsed_keys_;

  TF_DISALLOW_COPY_AND_ASSIGN(XlaSendFromHostOp);
};

REGISTER_XLA_OP(Name("XlaHostCompute"), XlaHostComputeOp);

REGISTER_KERNEL_BUILDER(Name("_XlaRecvAtHost").Device(DEVICE_CPU),
                        XlaRecvAtHostOp);

REGISTER_KERNEL_BUILDER(Name("_XlaSendFromHost").Device(DEVICE_CPU),
                        XlaSendFromHostOp);

}  // namespace tensorflow
