# Copyright 2020 The TensorFlow Authors. All Rights Reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
# ==============================================================================
"""
Keras API
~~~~~~~~~
"""

from tensorflow.python.ipu.keras.extensions import PipelineStage
from tensorflow.python.ipu.keras.extensions import FunctionalLayerPipelineStageAssignment
from tensorflow.python.ipu.keras.extensions import FunctionalExtension
from tensorflow.python.ipu.keras.extensions import SequentialLayerPipelineStageAssignment
from tensorflow.python.ipu.keras.extensions import SequentialExtension
from tensorflow.python.ipu.keras import layers
from tensorflow.python.ipu.keras.model import Model, Sequential
from tensorflow.python.ipu.keras.pipeline import PipelineModel
from tensorflow.python.ipu.keras.pipeline import PipelineSequential
from tensorflow.python.ipu.keras.losses import CTCLoss
