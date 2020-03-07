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
# =============================================================================
from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import json
import tempfile
import glob
import os
import subprocess
import sys

import numpy as np
import tensorflow as tf
from tensorflow.compiler.tests import xla_test
from tensorflow.python.platform import googletest
from tensorflow.python.framework import ops
from tensorflow.compat.v1.train import Saver
from tensorflow.python.framework import test_util
from tensorflow.python.ipu import utils
from tensorflow.python.platform import tf_logging as logging
from tensorflow.python.platform import test
from tensorflow.python.ops.variables import global_variables_initializer


def filesInFolder(folder):
  return [
      name for name in os.listdir(folder)
      if os.path.isfile(os.path.join(folder, name))
  ]


class MyInitializer:
  def __init__(self, value):
    self.value = value

  def __call__(self, shape, dtype=None):
    assert dtype in [None, tf.float32]

    def generator(*args):
      return self.value + sum([10 * idx + v for idx, v in enumerate(args)])

    return np.fromfunction(generator, shape)


def instantiate_lenet():
  from tensorflow.python import keras
  from tensorflow.python.keras import layers
  model = keras.Sequential()

  model.add(
      layers.Conv2D(filters=6,
                    kernel_size=(3, 3),
                    activation='relu',
                    input_shape=(32, 32, 1)))
  model.add(layers.AveragePooling2D())
  model.add(layers.Conv2D(filters=16, kernel_size=(3, 3), activation='relu'))
  model.add(layers.AveragePooling2D())
  model.add(layers.Flatten())
  model.add(layers.Dense(units=120, activation='relu'))
  model.add(layers.Dense(units=84, activation='relu'))
  model.add(layers.Dense(units=10, activation='softmax'))

  inp = keras.Input(shape=(32, 32, 1), dtype=np.float32)
  out = model(inp)
  return out, inp, model


def instantiate_lenet_fix_weights():
  from tensorflow.python import keras
  from tensorflow.python.keras import layers
  model = keras.Sequential()

  model.add(
      layers.Conv2D(filters=6,
                    kernel_size=(3, 3),
                    activation='relu',
                    input_shape=(32, 32, 1),
                    kernel_initializer=MyInitializer(10.0)))
  model.add(layers.AveragePooling2D())
  model.add(
      layers.Conv2D(filters=16,
                    kernel_size=(3, 3),
                    activation='relu',
                    kernel_initializer=MyInitializer(20.0)))
  model.add(layers.AveragePooling2D())
  model.add(layers.Flatten())
  model.add(
      layers.Dense(units=120,
                   activation='relu',
                   kernel_initializer=MyInitializer(30.0)))
  model.add(
      layers.Dense(units=84,
                   activation='relu',
                   kernel_initializer=MyInitializer(0.4)))
  model.add(
      layers.Dense(units=10,
                   activation='softmax',
                   kernel_initializer=MyInitializer(5.5)))

  inp = keras.Input(shape=(32, 32, 1), dtype=np.float32)
  out = model(inp)
  return out, inp, model


class PoplarExecutableRunnerTest(xla_test.XLATestCase):
  # Overriding abstract method.
  def cached_session(self):
    return 0

  # Overriding abstract method.
  def test_session(self):
    return 0

  def configureIPU(self, serialization_folder=None, offline_compilation=True):
    opts = utils.create_ipu_config()
    if offline_compilation:
      opts = utils.set_ipu_connection_type(opts,
                                           utils.DeviceConnectionType.NEVER, 1)
    if serialization_folder:
      opts = utils.set_serialization_options(opts, serialization_folder)
    utils.configure_ipu_system(opts)

  def runCommand(self, cmd):
    logging.info("Running: %s", " ".join(cmd))
    out = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    self.assertTrue(out.returncode == 0, out.stdout.decode("utf-8"))
    logging.info(out.stdout.decode("utf-8"))

  def runPythonCommand(self, cmd):
    python_cmd = cmd
    python_cmd.insert(0, sys.executable)
    self.runCommand(python_cmd)

  def getSingleFileWithExt(self, folder, extension):
    all_files = glob.glob("%s/*.%s" % (folder, extension))
    logging.info("%s files in %s: %s", extension, folder, all_files)
    self.assertEqual(
        len(all_files), 1,
        "There should be exactly one file with the extension %s in %s: %s" %
        (extension, folder, all_files))
    return all_files[0]

  @test_util.deprecated_graph_mode_only
  def testKerasLenet(self):
    """Check that the output of PoplarExecutableRunner produces the same output as the original Graph execution.
    """
    if utils.running_on_ipu_model():
      self.skipTest("PoplarExecutableRunner only works with physical IPUs")

    with tempfile.TemporaryDirectory() as tmp:
      poplar_binaries_folder = os.path.join(tmp, "poplar")
      model_path = os.path.join(tmp, "model")
      weights_path = os.path.join(tmp, "weights")
      output_path = os.path.join(tmp, "output")
      input_values = np.random.uniform(size=(1, 32, 32, 1))
      input_file = "%s/input.data" % tmp

      with open(input_file, 'w') as f:
        json.dump(input_values.tolist(), f)

      with self.session() as sess:
        self.configureIPU(poplar_binaries_folder, False)
        with ops.device("/device:IPU:0"):
          out, inp, model = instantiate_lenet()

        utils.move_variable_initialization_to_cpu()
        sess.run(global_variables_initializer())

        # Run the model once to generate the poplar binaries.
        reference_values = sess.run(out, {inp: input_values})

        # Export the model & weights.
        tf.saved_model.save(model, model_path)

      metadata_file = self.getSingleFileWithExt(poplar_binaries_folder, "json")
      executable_file = self.getSingleFileWithExt(poplar_binaries_folder,
                                                  "poplar_exec")

      self.runPythonCommand(
          (("./tensorflow/compiler/plugin/poplar/tools/"
            "tensorflow_weights_extractor.py -o %s -s %s -m %s") %
           (weights_path, model_path, metadata_file)).split())

      self.runCommand(
          (("./tensorflow/compiler/plugin/poplar/tools/PoplarExecutableRunner"
            " --model_executable %s --model_metadata %s --weights_path %s "
            "--output_folder=%s --input_data=input_1=%s --strict") %
           (executable_file, metadata_file, weights_path, output_path,
            input_file)).split())

      output_file = self.getSingleFileWithExt(output_path, "data")
      with open(output_file, 'r') as f:
        runner_values = np.array(json.load(f))
        logging.info("Reference %s\nRunner: %s", reference_values,
                     runner_values)
        self.assertAllClose(reference_values, runner_values)

  @test_util.deprecated_graph_mode_only
  @test_util.run_v2_only
  def testWeightsExportersNoMetadata(self):
    """ Check that the weights extractor produces the same output with
     TF v1 and v2 models."""
    # Disable the IPU model
    poplar_flags = os.environ.get("TF_POPLAR_FLAGS",
                                  "").replace("--use_ipu_model", "")
    with test.mock.patch.dict("os.environ",
                              {"TF_POPLAR_FLAGS": poplar_flags
                               }), tempfile.TemporaryDirectory() as tmp:
      model_path_keras = os.path.join(tmp, "model_keras")
      model_path_session = os.path.join(tmp, "model_session")
      weights_path_keras = os.path.join(tmp, "weights_keras")
      weights_path_session = os.path.join(tmp, "weights_session")

      with self.session() as sess:
        self.configureIPU()
        with ops.device("/device:IPU:0"):
          _, _, model = instantiate_lenet()
        utils.move_variable_initialization_to_cpu()
        sess.run(global_variables_initializer())

        # Export the model & weights.
        tf.saved_model.save(model, model_path_keras)
        Saver().save(sess, model_path_session)

      self.runPythonCommand((("./tensorflow/compiler/plugin/poplar/tools/"
                              "tensorflow_weights_extractor.py -o %s -s %s") %
                             (weights_path_keras, model_path_keras)).split())

      self.runPythonCommand(
          (("./tensorflow/compiler/plugin/poplar/tools/"
            "tensorflow_weights_extractor.py -o %s -s %s") %
           (weights_path_session, model_path_session)).split())

      keras_files = sorted(glob.glob("%s/*" % weights_path_keras))
      session_files = sorted(glob.glob("%s/*" % weights_path_session))
      logging.info("Keras weights files: %s" % keras_files)
      logging.info("Session weights files: %s" % session_files)
      self.assertEqual(len(keras_files), len(session_files))
      for idx, keras_file in enumerate(keras_files):
        session_file = session_files[idx]
        self.assertEqual(os.path.basename(session_file),
                         os.path.basename(keras_file))
        with open(session_file, 'r') as s, open(keras_file, 'r') as k:
          self.assertEqual(s.read(), k.read())

  @test_util.deprecated_graph_mode_only
  def testWeightsExportersMetadataLive(self):
    """Export weights directly from a live model.
    """
    poplar_flags = os.environ.get("TF_POPLAR_FLAGS",
                                  "").replace("--use_ipu_model", "")
    with test.mock.patch.dict("os.environ",
                              {"TF_POPLAR_FLAGS": poplar_flags
                               }), tempfile.TemporaryDirectory() as tmp:
      poplar_binaries_folder = os.path.join(tmp, "poplar")
      weights_path_keras = os.path.join(tmp, "weights_keras")
      weights_path_session = os.path.join(tmp, "weights_session")

      with self.session() as sess:
        self.configureIPU(poplar_binaries_folder)
        with ops.device("/device:IPU:0"):
          out, inp, model = instantiate_lenet_fix_weights()

        utils.move_variable_initialization_to_cpu()
        sess.run(global_variables_initializer())

        # Run the model once to generate the poplar binaries.
        try:
          sess.run(out, {inp: np.ones((1, 32, 32, 1))})
        except tf.python.framework.errors_impl.InvalidArgumentError:
          pass

      metadata_file = self.getSingleFileWithExt(poplar_binaries_folder, "json")

      with self.session() as sess:
        self.configureIPU()
        with ops.device("/device:IPU:0"):
          _, _, _ = instantiate_lenet_fix_weights()

        utils.move_variable_initialization_to_cpu()
        sess.run(global_variables_initializer())

        utils.export_variables_from_live_session(sess, weights_path_session,
                                                 metadata_file)

      with self.session() as sess:
        self.configureIPU()
        with ops.device("/device:IPU:0"):
          _, _, model = instantiate_lenet_fix_weights()

        utils.move_variable_initialization_to_cpu()
        sess.run(global_variables_initializer())
        utils.export_variables_from_live_model(model, weights_path_keras,
                                               metadata_file)

      keras_files = sorted(glob.glob("%s/*" % weights_path_keras))
      session_files = sorted(glob.glob("%s/*" % weights_path_session))
      logging.info("Keras weights files: %s" % keras_files)
      logging.info("Session weights files: %s" % session_files)
      self.assertEqual(len(keras_files), 10)
      self.assertEqual(len(session_files), 10)
      for idx, keras_file in enumerate(keras_files):
        session_file = session_files[idx]
        self.assertEqual(os.path.basename(session_file),
                         os.path.basename(keras_file))
        with open(session_file, 'r') as s, open(keras_file, 'r') as k:
          self.assertEqual(s.read(), k.read())


if __name__ == "__main__":
  os.environ['TF_XLA_FLAGS'] = ('--tf_xla_min_cluster_size=1' +
                                os.environ.get('TF_XLA_FLAGS', ''))
  googletest.main()
