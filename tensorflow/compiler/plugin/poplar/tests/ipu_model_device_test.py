# Copyright 2017 Graphcore Ltd
#

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import numpy as np
import re

import test_utils as tu

from tensorflow.compiler.plugin.poplar.driver.config_pb2 import IpuOptions
from tensorflow.compiler.plugin.poplar.driver.trace_pb2 import IpuTraceEvent
from tensorflow.compiler.plugin.poplar.ops import gen_ipu_ops
from tensorflow.core.protobuf import config_pb2
from tensorflow.python.client import session as session_lib
from tensorflow.python.platform import googletest
from tensorflow.python.framework import errors
from tensorflow.python.framework import ops
from tensorflow.python.framework import test_util
from tensorflow.python.ops import array_ops
from tensorflow.python.ops import math_ops


class IpuIpuModelTest(test_util.TensorFlowTestCase):
  def testIpuModelDevice(self):
    with ops.device("/device:IPU:0"):
      pa = array_ops.placeholder(np.float32, [2, 2], name="a")
      pb = array_ops.placeholder(np.float32, [2, 2], name="b")
      output = pa + pb

    tu.configure_ipu_system()

    with session_lib.Session() as sess:
      fd = {pa: [[1., 1.], [2., 3.]], pb: [[0., 1.], [4., 5.]]}
      result = sess.run(output, fd)
      self.assertAllClose(result, [[1., 2.], [6., 8.]])

  def testIpuModelDeviceWithNoReport(self):
    with ops.device("/device:IPU:0"):
      pa = array_ops.placeholder(np.float32, [2, 2], name="a")
      pb = array_ops.placeholder(np.float32, [2, 2], name="b")
      output = pa + pb

    with ops.device('cpu'):
      with ops.control_dependencies([output]):
        report = gen_ipu_ops.ipu_event_trace()

    tu.configure_ipu_system(False, False, False)

    with session_lib.Session() as sess:
      fd = {pa: [[1., 1.], [2., 3.]], pb: [[0., 1.], [4., 5.]]}
      sess.run(report, fd)

      result, rep = sess.run([output, report], fd)
      self.assertAllClose(result, [[1., 2.], [6., 8.]])
      self.assertTrue(len(rep) == 0)

  def testIpuModelDeviceWithReport(self):
    with ops.device("/device:IPU:0"):
      pa = array_ops.placeholder(np.float32, [2, 2], name="a")
      pb = array_ops.placeholder(np.float32, [2, 2], name="b")
      output = pa + pb

    with ops.device('cpu'):
      with ops.control_dependencies([output]):
        report = gen_ipu_ops.ipu_event_trace()

    tu.configure_ipu_system()

    with session_lib.Session() as sess:
      fd = {pa: [[1., 1.], [2., 3.]], pb: [[0., 1.], [4., 5.]]}
      sess.run(report, fd)

      result, rep = sess.run([output, report], fd)
      self.assertAllClose(result, [[1., 2.], [6., 8.]])
      self.assertEqual(len(rep), 3)
      evts = tu.extract_all_events(rep)
      self.assertEqual(evts[0].type, IpuTraceEvent.COMPILE_BEGIN)
      self.assertEqual(evts[1].type, IpuTraceEvent.COMPILE_END)
      self.assertEqual(evts[2].type, IpuTraceEvent.EXECUTE)

  def testIpuModelDeviceWithMultipleReport(self):
    with ops.device("/device:IPU:0"):
      pa = array_ops.placeholder(np.float32, [2, 2], name="a")
      pb = array_ops.placeholder(np.float32, [2, 2], name="b")
      out1 = pa + pb
      out2 = pa - pb

    with ops.device('cpu'):
      with ops.control_dependencies([out1, out2]):
        report = gen_ipu_ops.ipu_event_trace()

    tu.configure_ipu_system()

    with session_lib.Session() as sess:
      fd = {pa: [[1., 1.], [2., 3.]], pb: [[0., 1.], [4., 5.]]}
      sess.run(report, fd)

      result = sess.run(out1, fd)
      self.assertAllClose(result, [[1., 2.], [6., 8.]])

      result, rep = sess.run([out2, report], fd)
      self.assertAllClose(result, [[1., 0.], [-2., -2.]])

      # 2x compile_begin, 2x compile_end, 2x load engine
      self.assertEqual(len(rep), 6)

  def testEngineCompilationOptions(self):
    with ops.device("/device:IPU:0"):
      pa = array_ops.placeholder(np.float32, [480], name="a")
      pb = array_ops.placeholder(np.float32, [480], name="b")
      output = pa + pb

    tu.configure_ipu_system(
        True, True, True, engine_opts={"some_option": "some_value"})

    try:
      with session_lib.Session() as sess:
        fd = {pa: np.zeros([480]), pb: np.zeros([480])}
        sess.run(output, fd)

        self.assertTrue(False)
    except errors.InvalidArgumentError:
      pass

  def testNamedOperations(self):
    with ops.device("/device:IPU:0"):
      pa = array_ops.placeholder(np.float32, [2, 2], name="a")
      pb = array_ops.placeholder(np.float32, [2, 2], name="b")
      with ops.name_scope('my_ops'):
        out = math_ops.add(pa, pb, 'my_add_op')

    with ops.device('cpu'):
      report = gen_ipu_ops.ipu_event_trace()

    tu.configure_ipu_system()

    with tu.ipu_session() as sess:
      fd = {pa: [[1., 1.], [2., 3.]], pb: [[0., 1.], [4., 5.]]}
      sess.run(report, fd)

      result = sess.run(out, fd)
      self.assertAllClose(result, [[1., 2.], [6., 8.]])

      rep = sess.run(report, fd)
      s = tu.extract_all_strings_from_event_trace(rep)
      cs_list = tu.get_compute_sets_from_report(s)

      ok = ['__seed*', 'my_ops/my_add_op/add']

      self.assertTrue(tu.check_all_compute_sets_and_list(cs_list, ok))

  def testReportEveryNthExecution(self):
    with ops.device("/device:IPU:0"):
      pa = array_ops.placeholder(np.float32, [2, 2], name="a")
      pb = array_ops.placeholder(np.float32, [2, 2], name="b")
      out = math_ops.add(pa, pb)

    with ops.device('cpu'):
      report = gen_ipu_ops.ipu_event_trace()

    tu.configure_ipu_system(compilation_trace=False)

    with tu.ipu_session() as sess:
      fd = {pa: [[1., 1.], [2., 3.]], pb: [[0., 1.], [4., 5.]]}
      sess.run(report, fd)

      sess.run(out, fd)
      sess.run(out, fd)
      sess.run(out, fd)
      sess.run(out, fd)
      sess.run(out, fd)

      rep = sess.run(report, fd)
      evts = tu.extract_all_execute_events(rep)
      self.assertEqual(len(evts), 5)  # execute x 5

      for i, e in enumerate(evts):
        if i > 0:
          self.assertTrue(len(e.execute.execution_report) == 0)

      sess.close()

    tu.configure_ipu_system(
        compilation_trace=False, report_every_nth_execution=2)

    with tu.ipu_session() as sess:
      fd = {pa: [[1., 1.], [2., 3.]], pb: [[0., 1.], [4., 5.]]}
      sess.run(report, fd)

      sess.run(out, fd)
      sess.run(out, fd)
      sess.run(out, fd)
      sess.run(out, fd)
      sess.run(out, fd)

      rep = sess.run(report, fd)
      evts = tu.extract_all_execute_events(rep)
      self.assertEqual(len(evts), 5)  # execute x 5

      for i, e in enumerate(evts):
        if i % 2 != 0:
          self.assertTrue(len(e.execute.execution_report) == 0)

      sess.close()

    tu.configure_ipu_system(
        compilation_trace=False, report_every_nth_execution=1)

    with tu.ipu_session() as sess:
      fd = {pa: [[1., 1.], [2., 3.]], pb: [[0., 1.], [4., 5.]]}
      sess.run(report, fd)

      sess.run(out, fd)
      sess.run(out, fd)
      sess.run(out, fd)
      sess.run(out, fd)
      sess.run(out, fd)

      rep = sess.run(report, fd)
      evts = tu.extract_all_execute_events(rep)
      self.assertEqual(len(evts), 5)  # execute x 5

      for e in evts:
        self.assertTrue(len(e.execute.execution_report) > 0)

      sess.close()

  def testJsonReport(self):
    with ops.device("/device:IPU:0"):
      pa = array_ops.placeholder(np.float32, [2, 2], name="a")
      pb = array_ops.placeholder(np.float32, [2, 2], name="b")
      out = math_ops.add(pa, pb)

    with ops.device('cpu'):
      report = gen_ipu_ops.ipu_event_trace()

    tu.configure_ipu_system(text_report=False)

    with tu.ipu_session() as sess:
      fd = {pa: [[1., 1.], [2., 3.]], pb: [[0., 1.], [4., 5.]]}
      sess.run(report, fd)

      sess.run(out, fd)

      rep = sess.run(report, fd)
      evts = tu.extract_all_events(rep)
      self.assertEqual(len(evts), 3)  # begin, end, execute

      self.assertEqual(
          evts[1].compile_end.compilation_report.decode('utf-8')[0], '{')
      self.assertEqual(evts[2].execute.execution_report.decode('utf-8')[0],
                       '{')

  def testCborReport(self):
    with ops.device("/device:IPU:0"):
      pa = array_ops.placeholder(np.float32, [2, 2], name="a")
      pb = array_ops.placeholder(np.float32, [2, 2], name="b")
      out = math_ops.add(pa, pb)

    with ops.device('cpu'):
      report = gen_ipu_ops.ipu_event_trace()

    tu.configure_ipu_system(text_report=False, cbor_report=True)

    with tu.ipu_session() as sess:
      fd = {pa: [[1., 1.], [2., 3.]], pb: [[0., 1.], [4., 5.]]}
      sess.run(report, fd)

      sess.run(out, fd)

      rep = sess.run(report, fd)
      evts = tu.extract_all_events(rep)
      self.assertEqual(len(evts), 3)  # begin, end, execute

      self.assertEqual(evts[1].compile_end.compilation_report[0],
                       bytes(bytearray([217])))
      self.assertEqual(evts[2].execute.execution_report[0],
                       bytes(bytearray([217])))

  def testIpuEventsWithoutPoplarReporting(self):
    with ops.device("/device:IPU:0"):
      pa = array_ops.placeholder(np.float32, [2, 2], name="a")
      pb = array_ops.placeholder(np.float32, [2, 2], name="b")
      out = math_ops.add(pa, pb)

    with ops.device('cpu'):
      report = gen_ipu_ops.ipu_event_trace()

    tu.configure_ipu_system(
        enable_ipu_events=True,
        compilation_trace=False,
        io_trace=False,
        execution_trace=False)

    with tu.ipu_session() as sess:
      fd = {pa: [[1., 1.], [2., 3.]], pb: [[0., 1.], [4., 5.]]}
      sess.run(report, fd)

      sess.run(out, fd)

      rep = sess.run(report, fd)
      evts = tu.extract_all_events(rep)
      self.assertEqual(len(evts), 3)  # compile begin, compile end, execute

      for e in evts:
        if e.type == IpuTraceEvent.COMPILE_END:
          self.assertTrue(len(e.compile_end.compilation_report) == 0)
        if e.type == IpuTraceEvent.EXECUTE:
          self.assertTrue(len(e.execute.execution_report) == 0)

      sess.close()


if __name__ == "__main__":
  googletest.main()
