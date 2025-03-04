# Copyright 2019 The TensorFlow Authors. All Rights Reserved.
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
"""
Outfeed queue
~~~~~~~~~~~~~
"""

from enum import Enum
import collections
import threading

from tensorflow.compiler.plugin.poplar.ops import gen_pop_datastream_ops
from tensorflow.compiler.plugin.poplar.ops import gen_poputil_ops
from tensorflow.python.distribute import distribution_strategy_context as ds_context
from tensorflow.python.eager import context
from tensorflow.python.framework import func_graph
from tensorflow.python.framework import ops
from tensorflow.python.ipu import ipu_strategy
from tensorflow.python.ops import array_ops
from tensorflow.python.ops import control_flow_ops
from tensorflow.python.util import nest, deprecation
from tensorflow.python.util.compat import collections_abc

_uid_counter = 0
_uid_lock = threading.Lock()


def _generate_unique_name():
  with _uid_lock:
    global _uid_counter
    uid = _uid_counter
    _uid_counter += 1
  return str(uid)


class IPUOutfeedMode(Enum):
  """Types used to control the IPUOutfeedQueue modes.

  Contains the following values:

  * `ALL` - When used with an IPUOutfeedQueue, all the elements which were
    enqueued to the queue will be returned by the outfeed.
  * `LAST` - When used with an IPUOutfeedQueue, only the last element which was
    enqueued to the queue will be returned by the outfeed.

  """
  ALL = "all"
  LAST = "get_last"


class IPUOutfeedQueue(collections_abc.Iterable):
  """Generates and adds outfeed enqueue/dequeue operations to the graph.

  An outfeed is the counterpart to an infeed and manages the
  transfer of data (like tensors, tuples or dictionaries of tensors)
  from the IPU graph to the host.

  The queue has two modes of operation - outfeed all or outfeed last.
  In outfeed all mode every element that is enqueued will be stored
  for a subsequent dequeue. All of the enqueued elements will be returned
  when the dequeue operation is run. This is the default behaviour.

  In outfeed last mode only the last enqueued element is stored. The dequeue
  operation will in this case return a single element.

  """

  _replication_factor_deprecated_instructions = """No change needed.
  replication_factor is now set automatically based on the model."""
  _io_batch_size_deprecated_instructions = """Accumulate the results manually or
  use accumulate_outfeed when using pipelining."""
  _feed_name_deprecated_instructions = """No change needed.
  feed_name is now automatically generated."""

  @deprecation.deprecated_args(None,
                               _replication_factor_deprecated_instructions,
                               "replication_factor")
  @deprecation.deprecated_args(None, _io_batch_size_deprecated_instructions,
                               "io_batch_size")
  @deprecation.deprecated_args(None, _feed_name_deprecated_instructions,
                               "feed_name")
  def __init__(
      self,
      feed_name=None,  # pylint: disable=unused-argument
      outfeed_mode=None,
      device_ordinal=None,
      replication_factor=None,  # pylint: disable=unused-argument
      io_batch_size=1):
    """Creates an IPUOutfeedQueue object.

    Args:
      outfeed_mode: `ipu_outfeed_queue.IPUOutfeedMode` type used to control the
        outfeed behaviour. If not specified then all elements will be
        returned by the outfeed when the dequeue operation is run.
      device_ordinal: Integer ordinal of the IPU device on which this queue will
        be used. If not specified will try and deduce the IPU device from the
        current strategy and if that fails will default to "/device:IPU:0".

    Raises:
      ValueError: if the types or values are incorrect
      """

    # Default to all.
    self._outfeed_mode = outfeed_mode or IPUOutfeedMode.ALL

    if not isinstance(self._outfeed_mode, IPUOutfeedMode):
      raise ValueError("Expected `outfeed_mode` value to be of "
                       "`ipu_outfeed_queue.IPUOutfeedMode` type, but is %s." %
                       (str(type(outfeed_mode))))

    if device_ordinal is None:
      strategy = ds_context.get_strategy()
      if isinstance(strategy, ipu_strategy.IPUStrategyV1):
        device_ordinal = strategy._device_ordinal  # pylint: disable=protected-access
      else:
        device_ordinal = 0

    if not isinstance(device_ordinal, int):
      raise ValueError('Device ordinal must be an integer')

    if device_ordinal < 0:
      raise ValueError('Device ordinal must be >= 0')

    self._outfeed_all = self._outfeed_mode == IPUOutfeedMode.ALL
    self._device_ordinal = device_ordinal
    self._feed_name = _generate_unique_name()

    self._operations = []
    self._structure = None
    self._device_str = '/device:IPU:{}'.format(str(device_ordinal))

    # Helper to handle async dequeue
    self._enqueuing_thread = None

  def enqueue(self, tensors):
    """Enqueue a tensor, tuple or a dictionary of tensors for being outfed
    from the IPU graph. This operation is placed on the IPU device.
    This function returns an Operation which needs be executed (by either
    returning it or using tf.control_dependencies(...))

    Examples:

    1. Outfeed returning a single tensor:

    .. code-block:: python

       outfeed_queue = ipu_outfeed_queue.IPUOutfeedQueue()

       def body(v):
         v = v + 1
         outfeed = outfeed_queue.enqueue(v)
         return (v, outfeed)

       def my_net(v):
         r = loops.repeat(20, body, (v))
         return r

       with ipu.scopes.ipu_scope("/device:IPU:0"):
         res = ipu_compiler.compile(my_net, inputs=[v])

       ...
       ...

    2. Outfeed returning a tuple of tensors:

    .. code-block:: python

       outfeed_queue = ipu_outfeed_queue.IPUOutfeedQueue()

       def body(v):
         v = v + 1
         x = v * 2
         outfeed = outfeed_queue.enqueue((v, x))
         return (v, outfeed)

       def my_net(v):
         r = loops.repeat(20, body, (v))
         return r

       with ipu.scopes.ipu_scope("/device:IPU:0"):
         res = ipu_compiler.compile(my_net, inputs=[v])

       ...
       ...

    3. Outfeed returning a dictionary of tensors:

    .. code-block:: python

       outfeed_queue = ipu_outfeed_queue.IPUOutfeedQueue()

       def body(v):
         v = v + 1
         x = v * 2
         outfeed = outfeed_queue.enqueue({"output_1": v,
                                          "output_2": x})
         return (v, outfeed)

       def my_net(v):
         r = loops.repeat(20, body, (v))
         return r

       with ipu.scopes.ipu_scope("/device:IPU:0"):
         res = ipu_compiler.compile(my_net, inputs=[v])

       ...
       ...

      """
    if self.enqueued:
      raise ValueError("An outfeed can only be enqueued once.")

    # Serialize the tensor structure and make sure all inputs are Tensor like.
    flat_tensors = nest.flatten(tensors)
    flat_tensors = ops.convert_n_to_tensor(flat_tensors)

    self._flat_types = [t.dtype for t in flat_tensors]
    self._flat_shapes = [t.get_shape() for t in flat_tensors]

    # Pack the tensor dtypes to represent the output structure.
    self._structure = nest.pack_sequence_as(tensors, self._flat_types)

    with ops.device(self._device_str):
      outfeed_op = gen_pop_datastream_ops.pop_datastream_outfeed_enqueue(
          flat_tensors,
          output_shapes=self._flat_shapes,
          outfeed_mode=self._outfeed_mode.value,
          feed_id=self._feed_name)

    self._operations.append(outfeed_op)
    self._enqueuing_thread = threading.get_ident()
    self._enqueuing_default_graph = ops.get_default_graph()
    return outfeed_op

  @property
  def enqueued(self):
    enqueued_graphs = set()

    def add_graphs(g):
      enqueued_graphs.add(g)
      if isinstance(g, func_graph.FuncGraph):
        # Consider all outer graphs enqueued as well.
        add_graphs(g.outer_graph)

    for o in self._operations:
      add_graphs(o.graph)

    # All threads have different default graphs. If we're checking if we
    # enqueued in a different thread to the one we enqueued in, the check will
    # always fail. Instead, in these cases, use the enqueuing thread's default
    # graph
    if self._enqueuing_thread is None:
      return False
    if threading.get_ident() != self._enqueuing_thread:
      current_graph = self._enqueuing_default_graph
    else:
      current_graph = ops.get_default_graph()
    return current_graph in enqueued_graphs

  def dequeue(self, wait_for_completion=False):
    """Generate host side operation to dequeue the outfeed values.

    Args:
      wait_for_completion: whether the dequeueing operation should wait for the
        current execution of a graph containing the outfeed enqueue to complete.
        Defaults to `False` which means that only the tensors which have already
        been enqueued will be returned.

    The return value of this operation depends on the enqueued tensors,
    replication factor and the execution mode. Where replication factor is
    determined by the model.

    Note: If the `TF_POPLAR_FLAGS` environment variable contains the flag
    `--use_synthetic_data` then no data will be returned to the host.
    If `outfeed_mode` is `IPUOutfeedMode.ALL` then empty arrays with the same
    element structure as the enqueued tensors are returned.
    If `outfeed_mode` is `IPUOutfeedMode.LAST` then running the dequeue
    operation will throw an exception (there is no last element in this case).

    Examples:

    1. Outfeed returning a single tensor:

    .. code-block:: python

        outfeed_queue = ipu_outfeed_queue.IPUOutfeedQueue()

        def body(input):
          output = input + 1
          outfeed = outfeed_queue.enqueue(output)
          return (output, outfeed)

        def my_net(input):
          r = loops.repeat(20, body, (input))
          return r

        with ipu.scopes.ipu_scope("/device:IPU:0"):
          res = ipu_compiler.compile(my_net, inputs=[v])

        with ops.device('cpu'):
          v = tf.placeholder(np.float32, [4, 4])

        outfeed = outfeed_queue.dequeue()
        with tf.Session() as sess:
          result = sess.run(res, {v:np.ones([4, 4], np.float32)})
          outfed = sess.run(outfeed)

    In this example the tensor `output` is of shape [4, 4] and it is enqueued
    into the outfeed. If the `outfeed_mode` is `IPUOutfeedMode.ALL`, and the
    model has a replication factor of 2 then the shape of the resulting
    `outfed` tensor will be [20, 2, 4, 4], where the first dimension represents
    the number of times we have enqueued a tensor to the outfeed - in this
    example the loop is repeated 20 times, and therefore we get 20 values back
    from the outfeed. The second dimension is the replication factor, which
    allows us to see the individual values from each replicated graph. If the
    `outfeed_mode` is `IPUOutfeedMode.LAST`, then the shape of
    the resulting `outfed` tensor will be [2, 4, 4], which represents the value
    of the output tensor the last time it was enqueued during execution for
    each of the replicated graphs.

    2. Outfeed returning a tuple of tensors:

    .. code-block:: python

        outfeed_queue = ipu_outfeed_queue.IPUOutfeedQueue()

        def body(input):
          output = input + 1
          sum = tf.reduce_sum(output)
          outfeed = outfeed_queue.enqueue((output, sum))
          return (output, outfeed)

        def my_net(input):
          r = loops.repeat(20, body, (input))
          return r

        with ipu.scopes.ipu_scope("/device:IPU:0"):
          res = ipu_compiler.compile(my_net, inputs=[v])

        with ops.device('cpu'):
          v = tf.placeholder(np.float32, [4, 4])

        outfeed = outfeed_queue.dequeue()
        with tf.Session() as sess:
          result = sess.run(res, {v:np.ones([4, 4], np.float32)})
          outfed = sess.run(outfeed)

    In this example we outfeed a tuple of tensors, `output` and `sum`, where
    the former is of shape [4, 4] and latter [1]. If the `outfeed_mode` is
    `IPUOutfeedMode.ALL` and the model has a replication factor of 1, then
    the resulting `outfed` is a two-tuple of tensors with shapes ([20, 4, 4],
    [20, 1]), where the first dimension in each of the tensors represents the
    number of times we have enqueued these tensors to the outfeed - in this
    example the loop is repeated 20 times, and therefore we get 20 values
    back from the outfeed for each of the tensors in the tuple. If the
    `outfeed_mode` is `IPUOutfeedMode.LAST`, then `outfed` is a two tuple of
    tensors with shapes ([4, 4], [1]), which represents the values of the
    `output` and `sum` tensors the last time they were enqueued during
    execution.

    Note that replication factor here is 1, which means that the extra
    replication dimension is not added.

    3. Outfeed returning a dictionary of tensors:

    .. code-block:: python

        outfeed_queue = ipu_outfeed_queue.IPUOutfeedQueue()

        def body(input):
          output = input + 1
          sum = tf.reduce_sum(output)
          outfeed = outfeed_queue.enqueue({"x": output,
                                           "y": sum})
          return (output, outfeed)

        def my_net(input):
          r = loops.repeat(40, body, (input))
          return r

        with ipu.scopes.ipu_scope("/device:IPU:0"):
          res = ipu_compiler.compile(my_net, inputs=[v])

        with ops.device('cpu'):
          v = tf.placeholder(np.float32, [4, 4])

        outfeed = outfeed_queue.dequeue()
        with tf.Session() as sess:
          result = sess.run(res, {v:np.ones([4, 4], np.float32)})
          outfed = sess.run(outfeed)

    In this example we outfeed a dictionary of tensors, `output` and `sum`,
    where the former is of shape [4, 4] and latter [1]. If the `outfeed_mode`
    is `IPUOutfeedMode.ALL` and the model has a replication factor of 8, then
    the resulting `outfed` is a dictionary of tensors with
    shapes: {"x": [40, 8, 4, 4], "y": [40, 8, 1]}, where the first dimension
    in each of the tensors represents the number of times we have enqueued
    these tensors to the outfeed - in this example the loop is repeated 40
    times, and therefore we get 40 values back from the outfeed for each of
    the tensors in the tuple. The second dimension is the replication factor,
    which allows us to see the individual values from each replicated graph.
    If the `outfeed_mode` is `IPUOutfeedMode.LAST`, then `outfed` is a
    dictionary of tensors with shapes: {"x": [8, 4, 4], "y": [8, 1]}, which
    represents the values of the `output` and `sum` tensors the last time
    they were enqueued during execution for each of the replicated graphs.

    """
    # Don't error/print the warning to the user if they try and dequeue from
    # another thread and the outfeed queue has not been populated yet.
    error_or_warn_when_unconnected = not (
        context.executing_eagerly() and
        (threading.get_ident() != self._enqueuing_thread))

    if not self.enqueued:
      if error_or_warn_when_unconnected:
        raise ValueError(
            "Trying to dequeue an outfeed which has not been enqueued.")
      return []

    def get_ipu_sync():
      with ops.device(self._device_str):
        return gen_poputil_ops.device_sync()

    # When executing eagerly, always make sure that the device has completed
    # before dequeueing on the same thread (asynchronous deaqueue is allowed to
    # access partial results).
    if context.executing_eagerly():
      wait_for_completion = wait_for_completion or (
          threading.get_ident() == self._enqueuing_thread)

    # Insert a device sync if required.
    if wait_for_completion:
      sync = get_ipu_sync()
    else:
      sync = control_flow_ops.no_op()

    with ops.control_dependencies([sync]):
      with ops.device('cpu'):
        outfeed_dequeue = \
          gen_pop_datastream_ops.pop_datastream_outfeed_dequeue(
              output_types=self._flat_types,
              output_shapes=self._flat_shapes,
              outfeed_mode=self._outfeed_mode.value,
              feed_id=self._feed_name,
              device_ordinal=self._device_ordinal,
              warn_when_unconnected=error_or_warn_when_unconnected)
    return nest.pack_sequence_as(self._structure, outfeed_dequeue)

  @property
  def deleter(self):
    """A `tf.Operation` that can be run to delete the resources owned
    by this IPUOutfeedQueue. This allows creating a new IPUOutfeedQueue
    with the same name afterwards. The behaviour is undefined if this
    op is executed concurrently with the dequeue op.

    Returns:
      A `tf.Operation` that can be run to delete this IPUOutfeedQueue
    """
    return gen_pop_datastream_ops.ipu_delete_outfeed(
        feed_id=self._feed_name, device_ordinal=self._device_ordinal)

  def __iter__(self):
    """Creates an iterator for elements of this dataset.

    The returned iterator implements the Python Iterator protocol.

    Returns:
      An `ipu.ipu_outfeed_queue.IPUOutfeedQueueIterator` for the elements
      enqueued into this outfeed.

    Raises:
      RuntimeError: If not executing eagerly or when the `outfeed_mode` is not
      `IPUOutfeedMode.ALL`.
    """
    if not context.executing_eagerly():
      raise RuntimeError(
          "IPUOutfeedQueue can only be iterated over in eager mode.")

    if self._outfeed_mode != IPUOutfeedMode.ALL:
      raise RuntimeError(
          "IPUOutfeedQueue can only be iterated over if the `outfeed_mode` is "
          "set to 'IPUOutfeedMode.ALL'.")

    return IPUOutfeedQueueIterator(self)


class IPUOutfeedQueueIterator(collections_abc.Iterator):
  """An iterator producing tf.Tensor objects from a IPUOutfeedQueue.
  """
  def __init__(self, outfeed_queue):
    """Creates a new iterator from the given outfeed queue.

    Args:
      outfeed_queue: A `ipu.ipu_outfeed_queue.IPUOutfeedQueue` object.
    """
    self._outfeed_queue = outfeed_queue
    self._results_to_process = collections.deque()
    self._num_elements = 0

  def __iter__(self):
    return self

  def __next__(self):
    if not self._num_elements:
      # Only try and get more results once all the existing results have been
      # processed.
      with context.eager_mode():
        results = self._outfeed_queue.dequeue()

      flat_results = nest.flatten(results)

      if not flat_results:
        raise StopIteration

      num_iterations = array_ops.shape(flat_results[0]).numpy()[0]
      if not num_iterations:
        raise StopIteration

      with context.eager_mode():
        # Populate the sliced results.
        self._num_elements += num_iterations
        for i in range(num_iterations):
          slice_results = [result[i] for result in flat_results]
          slice_results = nest.pack_sequence_as(results, slice_results)
          self._results_to_process.append(slice_results)

    if self._num_elements:
      self._num_elements -= 1
      return self._results_to_process.popleft()

    raise StopIteration


class ScopedIPUOutfeedQueue(IPUOutfeedQueue):
  """A version of IPUOutfeedQueue which automatically calls delete when it goes
  out of scope.

  Can only be created in eager mode.
  """
  def __init__(self, outfeed_mode=None, device_ordinal=None):
    """Creates an IPUOutfeedQueue object.

    Args:
      outfeed_mode: `ipu_outfeed_queue.IPUOutfeedMode` type used to control the
        outfeed behaviour. If not specified then all elements will be
        returned by the outfeed when the dequeue operation is run.
      device_ordinal: Integer ordinal of the IPU device on which this queue will
        be used. If not specified will try and deduce the IPU device from the
        current strategy and if that fails will default to "/device:IPU:0".

    Raises:
      RuntimeError: if not running in eager mode.
    """
    if not context.executing_eagerly():
      raise RuntimeError(
          "ScopedIPUOutfeedQueue can only be created in eager mode")

    super().__init__(outfeed_mode=outfeed_mode, device_ordinal=device_ordinal)

  def __del__(self):
    with context.eager_mode():
      gen_pop_datastream_ops.ipu_delete_outfeed(
          feed_id=self._feed_name,
          device_ordinal=self._device_ordinal,
          asynchronous=True)
