# Copyright 2014 The Chromium Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import json
import logging

from pylib.base import base_test_result


def GenerateResultsDict(test_run_result):
  """Create a results dict from |test_run_result| suitable for writing to JSON.
  Args:
    test_run_result: a base_test_result.TestRunResults object.
  Returns:
    A results dict that mirrors the one generated by
      base/test/launcher/test_results_tracker.cc:SaveSummaryAsJSON.
  """
  assert isinstance(test_run_result, base_test_result.TestRunResults)

  def status_as_string(s):
    if s == base_test_result.ResultType.PASS:
      return 'SUCCESS'
    elif s == base_test_result.ResultType.SKIP:
      return 'SKIPPED'
    elif s == base_test_result.ResultType.FAIL:
      return 'FAILURE'
    elif s == base_test_result.ResultType.CRASH:
      return 'CRASH'
    elif s == base_test_result.ResultType.TIMEOUT:
      return 'TIMEOUT'
    elif s == base_test_result.ResultType.UNKNOWN:
      return 'UNKNOWN'

  def generate_iteration_data(t):
    return {
      t.GetName(): [
        {
          'status': status_as_string(t.GetType()),
          'elapsed_time_ms': t.GetDuration(),
          'output_snippet': '',
          'losless_snippet': '',
          'output_snippet_base64:': '',
        }
      ]
    }

  all_tests_tuple, per_iteration_data_tuple = zip(
      *[(t.GetName(), generate_iteration_data(t))
        for t in test_run_result.GetAll()])

  return {
    'global_tags': [],
    'all_tests': list(all_tests_tuple),
    # TODO(jbudorick): Add support for disabled tests within base_test_result.
    'disabled_tests': [],
    'per_iteration_data': list(per_iteration_data_tuple),
  }


def GenerateJsonResultsFile(test_run_result, file_path):
  """Write |test_run_result| to JSON.

  This emulates the format of the JSON emitted by
  base/test/launcher/test_results_tracker.cc:SaveSummaryAsJSON.

  Args:
    test_run_result: a base_test_result.TestRunResults object.
    file_path: The path to the JSON file to write.
  """
  with open(file_path, 'w') as json_result_file:
    json_result_file.write(json.dumps(GenerateResultsDict(test_run_result)))

