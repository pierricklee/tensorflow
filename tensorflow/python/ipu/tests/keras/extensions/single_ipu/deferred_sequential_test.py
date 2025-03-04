# Copyright 2021 The TensorFlow Authors. All Rights Reserved.
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
"""Tests specific to deferred-build `Sequential` models."""

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import os
import unittest
import numpy as np

from tensorflow.python import keras
from tensorflow.python.compat import v2_compat
from tensorflow.python.keras import keras_parameterized
from tensorflow.python.keras import testing_utils
from tensorflow.python.ops import math_ops
from tensorflow.python.platform import test
from tensorflow.python import ipu

try:
  import h5py  # pylint:disable=g-import-not-at-top
except ImportError:
  h5py = None


class TestDeferredSequential(keras_parameterized.TestCase):
  def setUp(self):
    super(TestDeferredSequential, self).setUp()
    cfg = ipu.config.IPUConfig()
    cfg.auto_select_ipus = 1
    cfg.ipu_model.compile_ipu_code = False
    cfg.ipu_model.tiles_per_ipu = 1
    cfg.configure_ipu_system()
    self._ipu_strategy = ipu.ipu_strategy.IPUStrategyV1()
    self._ipu_strategy_scope = self._ipu_strategy.scope()
    self._ipu_strategy_scope.__enter__()

  def tearDown(self):
    self._ipu_strategy_scope.__exit__(None, None, None)
    super(TestDeferredSequential, self).tearDown()

  @keras_parameterized.run_all_keras_modes(always_skip_eager=True,
                                           always_skip_v1=True)
  def test_build_behavior(self):
    # Test graph network creation after __call__
    model = get_model()
    model(np.random.random((2, 6)))
    self.assertLen(model.weights, 4)
    self.assertTrue(model._is_graph_network)  # pylint:disable=protected-access
    self.assertLen(model.inputs, 1)
    self.assertLen(model.outputs, 1)
    self.assertEqual(model.inputs[0].shape.as_list(), [2, 6])
    self.assertEqual(model.outputs[0].shape.as_list(), [2, 2])

    # Test effect of new __call__ with a different shape
    model(np.random.random((3, 6)))
    self.assertLen(model.inputs, 1)
    self.assertLen(model.outputs, 1)
    self.assertEqual(model.inputs[0].shape.as_list(), [None, 6])
    self.assertEqual(model.outputs[0].shape.as_list(), [None, 2])
    model(np.random.random((4, 6)))
    self.assertLen(model.inputs, 1)
    self.assertLen(model.outputs, 1)
    self.assertEqual(model.inputs[0].shape.as_list(), [None, 6])
    self.assertEqual(model.outputs[0].shape.as_list(), [None, 2])

    # Test graph network creation after build
    model = get_model()
    model.build((None, 6))
    self.assertLen(model.weights, 4)
    self.assertTrue(model._is_graph_network)  # pylint:disable=protected-access
    self.assertLen(model.inputs, 1)
    self.assertLen(model.outputs, 1)
    self.assertEqual(model.inputs[0].shape.as_list(), [None, 6])
    self.assertEqual(model.outputs[0].shape.as_list(), [None, 2])

    # Test graph network creation after compile/fit
    model = get_model()
    model.compile(loss='mse',
                  optimizer='rmsprop',
                  metrics=[keras.metrics.CategoricalAccuracy()],
                  run_eagerly=testing_utils.should_run_eagerly())
    model.fit(np.zeros((32, 6)), np.zeros((32, 2)))
    self.assertLen(model.weights, 4)
    self.assertTrue(model._is_graph_network)  # pylint:disable=protected-access
    self.assertLen(model.inputs, 1)
    self.assertLen(model.outputs, 1)
    # Inconsistency here: with eager `fit`, the model is built with shape
    # (2, 6), but with graph function `fit`, it is built with shape `(None, 6)`.
    # This is likely due to our assumption "the batch size should be dynamic"
    # at the level of `Model`. TODO(fchollet): investigate and resolve.
    self.assertEqual(model.inputs[0].shape.as_list()[-1], 6)
    self.assertEqual(model.outputs[0].shape.as_list()[-1], 2)

  @keras_parameterized.run_all_keras_modes(always_skip_eager=True,
                                           always_skip_v1=True)
  def test_add_and_pop(self):
    model = get_model()
    model.build((None, 6))
    self.assertTrue(model.built)
    self.assertTrue(model._is_graph_network)  # pylint:disable=protected-access
    self.assertLen(model.layers, 3)
    self.assertLen(model.weights, 4)
    model.pop()
    self.assertTrue(model.built)
    self.assertTrue(model._is_graph_network)  # pylint:disable=protected-access
    self.assertLen(model.layers, 2)
    self.assertLen(model.weights, 2)
    model.add(keras.layers.Dense(2))
    self.assertTrue(model.built)
    self.assertTrue(model._is_graph_network)  # pylint:disable=protected-access
    self.assertLen(model.layers, 3)
    self.assertLen(model.weights, 4)

  @keras_parameterized.run_all_keras_modes(always_skip_eager=True,
                                           always_skip_v1=True)
  def test_saving_savedmodel(self):
    model = get_model()
    model(np.random.random((3, 6)))  # Build model

    path = os.path.join(self.get_temp_dir(), 'model_path')
    model.save(path)
    new_model = keras.models.load_model(path)
    for layer1, layer2 in zip(model._layers, new_model._layers):  # pylint:disable=protected-access
      self.assertEqual(layer1.name, layer2.name)
      for w1, w2 in zip(layer1.weights, layer2.weights):
        self.assertAllClose(w1, w2)

  @unittest.skipIf(h5py is None, 'Test requires h5py')
  @keras_parameterized.run_all_keras_modes(always_skip_eager=True,
                                           always_skip_v1=True)
  def test_saving_h5(self):
    path = os.path.join(self.get_temp_dir(), 'model_path.h5')
    model = get_model()
    model(np.random.random((3, 6)))  # Build model

    path = os.path.join(self.get_temp_dir(), 'model_path.h5')
    model.save(path)
    new_model = keras.models.load_model(path)
    for layer1, layer2 in zip(model._layers, new_model._layers):  # pylint:disable=protected-access
      self.assertEqual(layer1.name, layer2.name)
      for w1, w2 in zip(layer1.weights, layer2.weights):
        self.assertAllClose(w1, w2)

  @keras_parameterized.run_all_keras_modes(always_skip_eager=True,
                                           always_skip_v1=True)
  def test_loss_layer(self):
    class LossLayer(keras.layers.Layer):
      def call(self, inputs):  # pylint: disable=arguments-differ
        self.add_loss(math_ops.reduce_sum(inputs))
        return inputs

    # Test loss layer alone
    model = keras.Sequential([LossLayer()])
    model.compile('rmsprop', run_eagerly=testing_utils.should_run_eagerly())
    loss = model.train_on_batch(np.ones((2, 2)))
    self.assertAllClose(loss, 4.)
    model(np.random.random((4, 2)))  # Triggers a rebuild
    loss = model.train_on_batch(np.ones((1, 2)))
    self.assertAllClose(loss, 2.)

    # Test loss layer combined with another layer
    model = keras.Sequential(
        [keras.layers.Dense(1, kernel_initializer='ones'),
         LossLayer()])
    model.compile('rmsprop', run_eagerly=testing_utils.should_run_eagerly())
    loss = model.train_on_batch(np.ones((2, 2)))
    self.assertAllClose(loss, 4.)
    model(np.random.random((4, 2)))  # Triggers a rebuild
    loss = model.train_on_batch(np.ones((1, 2)))
    self.assertLess(loss, 2.)

    # Test loss layer combined with external loss
    model = keras.Sequential(
        [keras.layers.Dense(1, kernel_initializer='ones'),
         LossLayer()])
    model.compile('rmsprop',
                  'mse',
                  run_eagerly=testing_utils.should_run_eagerly())
    loss = model.train_on_batch(np.ones((2, 2)), np.ones((2, 2)))
    model(np.random.random((4, 2)))  # Triggers a rebuild
    loss = model.train_on_batch(np.ones((1, 2)), np.ones((1, 2)))


def get_model():
  model = keras.models.Sequential()
  model.add(keras.layers.Dense(2, name='first_layer'))
  model.add(keras.layers.Dropout(0.3, name='dp'))
  model.add(keras.layers.Dense(2, name='last_layer'))
  return model


if __name__ == '__main__':
  v2_compat.enable_v2_behavior()
  test.main()
