/*
 * Copyright (c) 2017, 2018, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "precompiled.hpp"

// This test performs mocking of certain JVM functionality. This works by
// including the source file under test inside an anonymous namespace (which
// prevents linking conflicts) with the mocked symbols redefined.

// The include list should mirror the one found in the included source file -
// with the ones that should pick up the mocks removed. Those should be included
// later after the mocks have been defined.

#include "jvm.h"
#include "classfile/classLoaderStats.hpp"
#include "classfile/javaClasses.hpp"
#include "code/codeCache.hpp"
#include "compiler/compileBroker.hpp"
#include "gc/g1/g1HeapRegionEventSender.hpp"
#include "gc/shared/gcConfiguration.hpp"
#include "gc/shared/gcTrace.hpp"
#include "gc/shared/objectCountEventSender.hpp"
#include "gc/shared/vmGCOperations.hpp"
#include "jfr/jfrEvents.hpp"
#include "jfr/periodic/jfrModuleEvent.hpp"
#include "jfr/periodic/jfrOSInterface.hpp"
#include "jfr/periodic/jfrThreadCPULoadEvent.hpp"
#include "jfr/periodic/jfrThreadDumpEvent.hpp"
#include "jfr/recorder/jfrRecorder.hpp"
#include "jfr/support/jfrThreadId.hpp"
#include "jfr/utilities/jfrTime.hpp"
#include "logging/log.hpp"
#include "memory/heapInspection.hpp"
#include "memory/resourceArea.hpp"
#include "oops/oop.inline.hpp"
#include "runtime/arguments.hpp"
#include "runtime/flags/jvmFlag.hpp"
#include "runtime/globals.hpp"
#include "runtime/os.hpp"
#include "runtime/os_perf.hpp"
#include "runtime/thread.inline.hpp"
#include "runtime/threadSMR.hpp"
#include "runtime/sweeper.hpp"
#include "runtime/vmThread.hpp"
#include "services/classLoadingService.hpp"
#include "services/management.hpp"
#include "services/threadService.hpp"
#include "utilities/exceptions.hpp"
#include "utilities/globalDefinitions.hpp"

#include "unittest.hpp"

namespace {

  class MockEventThreadCPULoad : public ::EventThreadCPULoad
  {
  public:
    float user;
    float system;

  public:
    MockEventThreadCPULoad(EventStartTime timing=TIMED) : ::EventThreadCPULoad(timing) {}

    void set_user(float new_value) {
      user = new_value;
    }
    void set_system(float new_value) {
      system = new_value;
    }
  };

  class MockOs : public ::os {
  public:
    static jlong user_cpu_time;
    static jlong system_cpu_time;

    static jlong thread_cpu_time(Thread *thread, bool user_sys_cpu_time) {
      return user_sys_cpu_time ? user_cpu_time + system_cpu_time : user_cpu_time;
    }
  };

  jlong MockOs::user_cpu_time;
  jlong MockOs::system_cpu_time;

// Reincluding source files in the anonymous namespace unfortunately seems to
// behave strangely with precompiled headers (only when using gcc though)
#ifndef DONT_USE_PRECOMPILED_HEADER
#define DONT_USE_PRECOMPILED_HEADER
#endif

#define os MockOs
#define EventThreadCPULoad MockEventThreadCPULoad

#include "jfrfiles/jfrPeriodic.hpp"
#include "jfr/periodic/jfrPeriodic.cpp"

#undef os
#undef EventThreadCPULoad

} // anonymous namespace

class JfrTestThreadCPULoadSingle : public ::testing::Test {
protected:
  JavaThread* thread;
  JfrThreadLocal* thread_data;
  MockEventThreadCPULoad event;

  void SetUp() {
    thread = new JavaThread();
    thread_data = thread->jfr_thread_local();
    thread_data->set_wallclock_time(0);
    thread_data->set_user_time(0);
    thread_data->set_cpu_time(0);
  }

  void TearDown() {
    delete thread;
  }
};

TEST_VM_F(JfrTestThreadCPULoadSingle, DISABLED_SingleCpu) {
  MockOs::user_cpu_time = 100 * NANOSECS_PER_MILLISEC;
  MockOs::system_cpu_time = 100 * NANOSECS_PER_MILLISEC;
  EXPECT_TRUE(JfrThreadCPULoadEvent::update_event(event, thread, 400 * NANOSECS_PER_MILLISEC, 1));
  EXPECT_FLOAT_EQ(0.25, event.user);
  EXPECT_FLOAT_EQ(0.25, event.system);
}

TEST_VM_F(JfrTestThreadCPULoadSingle, DISABLED_MultipleCpus) {
  MockOs::user_cpu_time = 100 * NANOSECS_PER_MILLISEC;
  MockOs::system_cpu_time = 100 * NANOSECS_PER_MILLISEC;
  EXPECT_TRUE(JfrThreadCPULoadEvent::update_event(event, thread, 400 * NANOSECS_PER_MILLISEC, 2));
  EXPECT_FLOAT_EQ(0.125, event.user);
  EXPECT_FLOAT_EQ(0.125, event.system);
}

TEST_VM_F(JfrTestThreadCPULoadSingle, DISABLED_BelowThreshold) {
  MockOs::user_cpu_time = 100;
  MockOs::system_cpu_time = 100;
  EXPECT_FALSE(JfrThreadCPULoadEvent::update_event(event, thread, 400 * NANOSECS_PER_MILLISEC, 2));
}

TEST_VM_F(JfrTestThreadCPULoadSingle, DISABLED_UserAboveMaximum) {

  // First call will not report above 100%
  MockOs::user_cpu_time = 200 * NANOSECS_PER_MILLISEC;
  MockOs::system_cpu_time = 100 * NANOSECS_PER_MILLISEC;
  EXPECT_TRUE(JfrThreadCPULoadEvent::update_event(event, thread, 200 * NANOSECS_PER_MILLISEC, 1));
  EXPECT_FLOAT_EQ(0.5, event.user);
  EXPECT_FLOAT_EQ(0.5, event.system);

  // Second call will see an extra 100 millisecs user time from the remainder
  EXPECT_TRUE(JfrThreadCPULoadEvent::update_event(event, thread, (200 + 400) * NANOSECS_PER_MILLISEC, 1));
  EXPECT_FLOAT_EQ(0.25, event.user);
  EXPECT_FLOAT_EQ(0, event.system);
}

TEST_VM_F(JfrTestThreadCPULoadSingle, DISABLED_SystemAboveMaximum) {

  // First call will not report above 100%
  MockOs::user_cpu_time = 100 * NANOSECS_PER_MILLISEC;
  MockOs::system_cpu_time = 300 * NANOSECS_PER_MILLISEC;
  EXPECT_TRUE(JfrThreadCPULoadEvent::update_event(event, thread, 200 * NANOSECS_PER_MILLISEC, 1));
  EXPECT_FLOAT_EQ(0, event.user);
  EXPECT_FLOAT_EQ(1, event.system);

  // Second call will see an extra 100 millisecs user and system time from the remainder
  EXPECT_TRUE(JfrThreadCPULoadEvent::update_event(event, thread, (200 + 400) * NANOSECS_PER_MILLISEC, 1));
  EXPECT_FLOAT_EQ(0.25, event.user);
  EXPECT_FLOAT_EQ(0.25, event.system);
}

TEST_VM_F(JfrTestThreadCPULoadSingle, DISABLED_SystemTimeDecreasing) {

  // As seen in an actual run - caused by different resolution for total and user time
  // Total time    User time    (Calculated system time)
  //       200          100         100
  //       210          200          10
  //       400          300         100

  MockOs::user_cpu_time = 100 * NANOSECS_PER_MILLISEC;
  MockOs::system_cpu_time = 100 * NANOSECS_PER_MILLISEC;
  EXPECT_TRUE(JfrThreadCPULoadEvent::update_event(event, thread, 400 * NANOSECS_PER_MILLISEC, 1));
  EXPECT_FLOAT_EQ(0.25, event.user);
  EXPECT_FLOAT_EQ(0.25, event.system);

  MockOs::user_cpu_time += 100 * NANOSECS_PER_MILLISEC;
  MockOs::system_cpu_time -= 90 * NANOSECS_PER_MILLISEC;
  EXPECT_TRUE(JfrThreadCPULoadEvent::update_event(event, thread, (400 + 400) * NANOSECS_PER_MILLISEC, 1));
  EXPECT_FLOAT_EQ(0.25, event.user);
  EXPECT_FLOAT_EQ(0, event.system);

  MockOs::user_cpu_time += 100 * NANOSECS_PER_MILLISEC;
  MockOs::system_cpu_time += 90 * NANOSECS_PER_MILLISEC;
  EXPECT_TRUE(JfrThreadCPULoadEvent::update_event(event, thread, (400 + 400 + 400) * NANOSECS_PER_MILLISEC, 1));
  EXPECT_FLOAT_EQ(0.25, event.user);
  EXPECT_FLOAT_EQ(0, event.system);
}
