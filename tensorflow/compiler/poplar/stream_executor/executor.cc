/* Copyright 2017 Graphcore Ltd
 */

/* Copyright 2016 The TensorFlow Authors. All Rights Reserved.

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

#include "tensorflow/compiler/poplar/stream_executor/executor.h"
#include "tensorflow/compiler/poplar/stream_executor/poplar_platform_id.h"

#include "tensorflow/compiler/xla/status_macros.h"

#include "tensorflow/core/lib/strings/stringprintf.h"

#include <string.h>
#include <dlfcn.h>

#include <poplar/Tensor.hpp>

namespace se = ::perftools::gputools;

namespace perftools {
namespace gputools {
namespace poplarplugin {

std::string
GetCopyHandle(int64 i) {
  static const std::string handles[10] = {
    "0", "1", "2", "3", "4", "5", "6", "7", "8", "9"};
  if (i < 10) return handles[i];
  return tensorflow::strings::Printf("%lld", i);
}

host::HostStream *AsPoplarStream(Stream *stream) {
  DCHECK(stream != nullptr);
  return dynamic_cast<host::HostStream *>(stream->implementation());
}

PoplarExecutor::PoplarExecutor(const PluginConfig &plugin_config)
    : plugin_config_(plugin_config) {
}

PoplarExecutor::~PoplarExecutor() {}

void *PoplarExecutor::Allocate(uint64 size) {
  void* raw_buf = new char[size + sizeof(TensorControl)];
  TensorControl* allocated = reinterpret_cast<TensorControl*>(raw_buf);
  allocated->on_device = false;
  allocated->input_handle = -1;
  allocated->output_handle = -1;
  {
    std::lock_guard<std::mutex> g(mutex_);
    allocations_.push_back(allocated);
  }
  return allocated;
}

void *PoplarExecutor::AllocateSubBuffer(DeviceMemoryBase *parent,
                                      uint64 offset_bytes, uint64 size_bytes) {
  TensorControl* tc = reinterpret_cast<TensorControl*>(parent->opaque());
  return tc->data + offset_bytes;
}

void PoplarExecutor::Deallocate(DeviceMemoryBase *mem) {
  if (!mem->is_sub_buffer()) {
    TensorControl* tc = reinterpret_cast<TensorControl*>(mem->opaque());
    {
      std::lock_guard <std::mutex> g(mutex_);
      allocations_.remove(tc);
    }
    delete[] static_cast<char *>(mem->opaque());
  }
}

bool PoplarExecutor::Memcpy(Stream *stream, void *host_dst,
                          const DeviceMemoryBase &pop_src, uint64 size) {
  AsPoplarStream(stream)->EnqueueTask(
      [this, host_dst, pop_src, size]() {
        port::Status ok = SynchronousMemcpy(host_dst, pop_src, size); });
  return true;
}

bool PoplarExecutor::Memcpy(Stream *stream, DeviceMemoryBase *pop_dst,
                          const void *host_src, uint64 size) {
  AsPoplarStream(stream)->EnqueueTask(
      [this, pop_dst, host_src, size]() {
        port::Status ok = SynchronousMemcpy(pop_dst, host_src, size); });
  return true;
}

port::Status PoplarExecutor::SynchronousMemcpy(DeviceMemoryBase *pop_dst,
                                             const void *host_src,
                                             uint64 size) {
  TensorControl* tc = reinterpret_cast<TensorControl*>(pop_dst->opaque());
  memcpy(tc->data, host_src, size);
  return port::Status::OK();
}

port::Status PoplarExecutor::SynchronousMemcpy(void *host_dst,
                                             const DeviceMemoryBase &pop_src,
                                             uint64 size) {
  const TensorControl* tc =
          reinterpret_cast<const TensorControl*>(pop_src.opaque());
  {
    std::lock_guard <std::mutex> g(mutex_);
    if (tc->on_device) {
      TF_RETURN_IF_ERROR(MoveDeviceToHost(const_cast<TensorControl*>(tc)));
    }
  }
  memcpy(host_dst, tc->data, size);
  return port::Status::OK();
}

bool PoplarExecutor::HostCallback(Stream *stream,
                                std::function<void()> callback) {
  AsPoplarStream(stream)->EnqueueTask(callback);
  return true;
}

bool PoplarExecutor::CreateStreamDependency(Stream *dependent, Stream *other) {
  AsPoplarStream(dependent)->EnqueueTask(
      [other]() { other->BlockHostUntilDone(); });
  AsPoplarStream(dependent)->BlockUntilDone();
  return true;
}

bool PoplarExecutor::StartTimer(Stream *stream, Timer *timer) {
  dynamic_cast<host::HostTimer *>(timer->implementation())->Start(stream);
  return true;
}

bool PoplarExecutor::StopTimer(Stream *stream, Timer *timer) {
  dynamic_cast<host::HostTimer *>(timer->implementation())->Stop(stream);
  return true;
}

bool PoplarExecutor::BlockHostUntilDone(Stream *stream) {
  AsPoplarStream(stream)->BlockUntilDone();
  return true;
}

DeviceDescription *PoplarExecutor::PopulateDeviceDescription() const {
  internal::DeviceDescriptionBuilder builder;

  builder.set_device_address_bits(64);

  // TODO populate dynamic values correctly
  builder.set_name("Poplar");
  builder.set_device_vendor("Graphcore");
  builder.set_platform_version("1.0");
  builder.set_driver_version("1.0");
  builder.set_runtime_version("1.0");
  builder.set_pci_bus_id("1");
  builder.set_device_memory_size(static_cast<uint64>(4) * 1024 * 1024 * 1024);
  builder.set_clock_rate_ghz(static_cast<float>(CLOCKS_PER_SEC) / 1e9);

  auto built = builder.Build();
  return built.release();
}

port::StatusOr<se::DeviceMemoryBase>
PoplarExecutor::AllocateOutputBuffer(const xla::Shape& shape) {

  if (shape.element_type() != xla::TUPLE) {
    uint64 size(xla::ShapeUtil::ByteSizeOf(shape));
    void* buf(Allocate(size));
    TensorControl* tc = reinterpret_cast<TensorControl*>(buf);
    tc->on_device = true;
    tc->output_handle = 0;
    return se::DeviceMemoryBase(buf, size);
  } else {
    int64 size(xla::ShapeUtil::ByteSizeOf(shape, sizeof(void*)));
    TensorControl* tc = reinterpret_cast<TensorControl*>(Allocate(size));

    void** buf = reinterpret_cast<void**>(tc->data);
    for (int64 n=0; n<xla::ShapeUtil::TupleElementCount(shape); n++) {
      const auto& s(shape.tuple_shapes(n));
      TensorControl* tc =
              reinterpret_cast<TensorControl*>(
                      Allocate(xla::ShapeUtil::ByteSizeOf(s)));
      tc->on_device = true;
      tc->output_handle = n;
      *buf++ = tc;
    }

    return se::DeviceMemoryBase(tc, size);
  }
}

port::Status
PoplarExecutor::MoveDeviceToHost(TensorControl* tc) const {
  if (tc->on_device == true && tc->output_handle != -1) {
    void* buf(static_cast<void*>(tc->data));
    current_engine_->readTensor(GetCopyHandle(tc->output_handle), buf);
    tc->on_device = false;
    tc->output_handle = -1;
    return port::Status::OK();
  } else {
    return tensorflow::errors::Internal("Tensor not on device");
  }
}

port::Status
PoplarExecutor::MoveHostToDevice(TensorControl* tc) const {
  if (tc->input_handle != -1) {
    void *buf(static_cast<void *>(tc->data));
    current_engine_->writeTensor(GetCopyHandle(tc->input_handle), buf);
    tc->on_device = true;
    return port::Status::OK();
  } else {
    return tensorflow::errors::Internal("Tensor not on host");
  }
}

port::StatusOr<se::DeviceMemoryBase>
PoplarExecutor::ExecuteEngine(Stream *stream,
                              poplar::Engine* engine,
                              const xla::Shape& shape,
                              const Args& args) {

  bool engine_changed(current_engine_ != engine);

  {
    std::lock_guard <std::mutex> g(mutex_);

    // Pull previous execution output back from device if:
    // a) the engine is changing
    // b) output buffer isn't an input to the current execution
    // c) output buffer isn't currently in the right place for the new input
    for (const auto& tc : allocations_) {
      if (tc->on_device == true && tc->output_handle != -1) {
        if (engine_changed) {
          TF_RETURN_IF_ERROR(MoveDeviceToHost(tc));
        } else if (tc->input_handle == -1) {
          TF_RETURN_IF_ERROR(MoveDeviceToHost(tc));
        } else if ((void*)tc != args[tc->input_handle].opaque()) {
          TF_RETURN_IF_ERROR(MoveDeviceToHost(tc));
        }
      }
    }

    current_engine_ = engine;

    // Put data on the device if:
    // a) the engine has changed
    // b) it is not on the device
    // c) it is on the device, but in the wrong place
    for (int64 a = 0; a < args.size(); a++) {
      auto mem = args[a];
      TensorControl *tc = reinterpret_cast<TensorControl *>(mem.opaque());
      if (tc->on_device == false || tc->input_handle != a || engine_changed) {
        tc->input_handle = a;
        TF_RETURN_IF_ERROR(MoveHostToDevice(tc));
      }
    }
  }

  perftools::gputools::DeviceMemoryBase retbuf;

  AsPoplarStream(stream)->EnqueueTask(
          [this, shape, &retbuf]() {
            TF_ASSIGN_OR_RETURN(retbuf, AllocateOutputBuffer(shape));
            current_engine_->run(0);
          });

  stream->BlockHostUntilDone();
  return retbuf;
}

std::string PoplarExecutor::GetPathToGraphProgFile() {
  Dl_info dlInfo;
  static const void* dummy;
  if (dladdr(&dummy, &dlInfo)) {
    std::string path(dlInfo.dli_fname);
    path = path.substr(0, path.find_last_of( '/' ) + 1);
    path = path + "../compiler/poplar/tf.gp";
    return path;
  }
  return "";
}



}  // namespace poplarplugin
}  // namespace gputools
}  // namespace perftools
