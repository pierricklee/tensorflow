/* Copyright 2017 The TensorFlow Authors. All Rights Reserved.

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

#ifndef TENSORFLOW_COMPILER_PLUGIN_POPLAR_DRIVER_TOOLS_SINGLE_HLO_MATCHER_H_
#define TENSORFLOW_COMPILER_PLUGIN_POPLAR_DRIVER_TOOLS_SINGLE_HLO_MATCHER_H_

#include "tensorflow/compiler/plugin/poplar/driver/tools/hlo_matcher.h"

namespace xla {

namespace poplarplugin {

// Template for all Fusing passes which match a single Poplibs op
class SingleHloMatcher : public HloMatcher {
 public:
  SingleHloMatcher(struct CompilerAnnotations& annotations,
                   const std::vector<HloMatcherPattern>& patterns,
                   std::string op_prefix,
                   bool restart_search_after_match = true)
      : HloMatcher(patterns, annotations, false, true,
                   /* look through max */ 3, restart_search_after_match),
        op_prefix_(op_prefix){};

  ~SingleHloMatcher() override = default;

 private:
  StatusOr<bool> HandleMatch(
      HloMatcherMatched& match,
      const absl::optional<int64> sharding_device) override;

  std::string op_prefix_;
};

}  // namespace poplarplugin
}  // namespace xla

#endif
