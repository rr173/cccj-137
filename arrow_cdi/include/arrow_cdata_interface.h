/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law but agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 *
 * ============================================================================
 * Apache Arrow C Data Interface, ABI-stable C definitions.
 *
 * Verbatim upstream definition (formatting only), source of truth:
 *   https://arrow.apache.org/docs/format/CDataInterface.html
 * Canonical header (Apache Arrow repository):
 *   https://github.com/apache/arrow/blob/main/cpp/src/arrow/c/abi.h
 *
 * Versioning contract (from the upstream documentation):
 *   "The Arrow C interface is defined in this header file. ... Any project
 *    can embed it and use it to exchange data in the Arrow format ...
 *    the structures have a version number ... release should set ... to 0."
 * Do not change field layout; only embed the upstream header.
 * ============================================================================
 */

#ifndef ARROW_C_DATA_INTERFACE
#define ARROW_C_DATA_INTERFACE

#include <stdint.h>

#define ARROW_FLAG_DICTIONARY_ORDERED 1
#define ARROW_FLAG_NULLABLE 2
#define ARROW_FLAG_MAP_KEYS_SORTED 4

struct ArrowSchema {
  const char* format;
  const char* name;
  const char* metadata;
  int64_t flags;
  int64_t n_children;
  struct ArrowSchema** children;
  struct ArrowSchema* dictionary;
  void (*release)(struct ArrowSchema*);
  void* private_data;
};

struct ArrowArray {
  int64_t length;
  int64_t null_count;
  int64_t offset;
  int64_t n_buffers;
  int64_t n_children;
  const void** buffers;
  struct ArrowArray** children;
  struct ArrowArray* dictionary;
  void (*release)(struct ArrowArray*);
  void* private_data;
};

/*
 * ArrowArrayStream is not used by this project: the requirement is the
 * C Data Interface (in-process batch/schema hand-off), not the C Stream
 * Interface. Kept out deliberately to keep the ABI surface minimal.
 */

#endif /* ARROW_C_DATA_INTERFACE */
