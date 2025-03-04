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
import tempfile
import os
from absl.testing import parameterized

from tensorflow.compiler.plugin.poplar.tests import test_utils as tu
from tensorflow.python import ipu
from tensorflow.python import keras
from tensorflow.python.data.ops import dataset_ops
from tensorflow.python.framework import test_util
from tensorflow.python.platform import test
from tensorflow.python.training import gradient_descent


def get_mnist_dataset(batch_size):
  mnist = keras.datasets.mnist

  (x_train, y_train), (_, _) = mnist.load_data()
  x_train = x_train / 255.0

  x_train = x_train.astype('float32')
  y_train = y_train.astype('int32')

  train_ds = dataset_ops.DatasetV2.from_tensor_slices(
      (x_train, y_train)).batch(batch_size, drop_remainder=True).repeat()

  return train_ds.repeat()


def simple_sequential_model():
  return keras.Sequential([
      keras.layers.Flatten(input_shape=(28, 28)),
      keras.layers.Dense(10,
                         activation='softmax',
                         kernel_initializer=keras.initializers.Constant(0.5),
                         bias_initializer='zeros')
  ])


def simple_functional_model():
  d = keras.layers.Input((28, 28))
  x = keras.layers.Flatten()(d)
  x = keras.layers.Dense(10,
                         activation='softmax',
                         kernel_initializer=keras.initializers.Constant(0.5),
                         bias_initializer='zeros')(x)
  return keras.Model(d, x)


class KerasGradientAccumulationTest(test.TestCase, parameterized.TestCase):
  TESTCASES = [{
      "testcase_name": "sequential",
      "model_fn": simple_sequential_model,
      "replication_factor": 1,
      "optimizer": "sgd"
  }, {
      "testcase_name": "sequential_replicated",
      "model_fn": simple_sequential_model,
      "replication_factor": 2,
      "optimizer": "sgd"
  }, {
      "testcase_name": "functional",
      "model_fn": simple_functional_model,
      "replication_factor": 1,
      "optimizer": "sgd"
  }, {
      "testcase_name": "functional_replicated",
      "model_fn": simple_functional_model,
      "replication_factor": 2,
      "optimizer": gradient_descent.GradientDescentOptimizer(0.001)
  }]

  @parameterized.named_parameters(*TESTCASES)
  @test_util.run_v2_only
  def testModels(self, model_fn, replication_factor, optimizer):
    tu.skip_if_not_enough_ipus(self, replication_factor)

    cfg = ipu.config.IPUConfig()
    cfg.auto_select_ipus = replication_factor
    tu.add_hw_ci_connection_options(cfg)
    cfg.configure_ipu_system()

    batch_size = 12
    gradient_accumulation_steps = 16
    gradient_accumulation_steps_per_replica = (gradient_accumulation_steps //
                                               replication_factor)
    steps_per_epoch = 64
    epochs = 2

    # Run on CPU - simulate gradient accumulation by just using a bigger batch
    # size but less steps per epoch.
    m = model_fn()
    m.compile(optimizer, loss=keras.losses.SparseCategoricalCrossentropy())
    m.fit(get_mnist_dataset(batch_size * gradient_accumulation_steps),
          steps_per_epoch=steps_per_epoch // gradient_accumulation_steps,
          epochs=epochs)
    cpu_weights = m.weights

    strategy = ipu.ipu_strategy.IPUStrategyV1()
    with strategy.scope():
      m = model_fn()
      m.compile(optimizer,
                loss=keras.losses.SparseCategoricalCrossentropy(),
                steps_per_execution=gradient_accumulation_steps * 2)
      m.set_gradient_accumulation_options(
          gradient_accumulation_steps_per_replica=
          gradient_accumulation_steps_per_replica,
          experimental_normalize_gradients=True)
      m.fit(get_mnist_dataset(batch_size),
            steps_per_epoch=steps_per_epoch,
            epochs=epochs)
      ipu_weights = m.weights

      self.assertAllClose(cpu_weights, ipu_weights)

      # Test that the extra properties are restored.
      with tempfile.TemporaryDirectory() as tmp:
        save_path = os.path.join(tmp, "model")
        m.save(save_path)
        m = keras.models.load_model(save_path)
        self.assertEqual(
            m._gradient_accumulation_steps_per_replica,  # pylint: disable=protected-access
            gradient_accumulation_steps_per_replica)
        self.assertEqual(
            m._experimental_gradient_accumulation_normalize_gradients, True)  # pylint: disable=protected-access
        self.assertFalse(m._gradient_accumulation_optimizer_kwargs)  # pylint: disable=protected-access


if __name__ == '__main__':
  test.main()
