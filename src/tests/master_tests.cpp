// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <unistd.h>

#include <memory>
#include <string>
#include <vector>

#include <gmock/gmock.h>

#include <mesos/executor.hpp>
#include <mesos/scheduler.hpp>

#include <mesos/allocator/allocator.hpp>

#include <mesos/scheduler/scheduler.hpp>

#include <process/clock.hpp>
#include <process/future.hpp>
#include <process/gmock.hpp>
#include <process/http.hpp>
#include <process/owned.hpp>
#include <process/pid.hpp>

#include <process/metrics/counter.hpp>
#include <process/metrics/metrics.hpp>

#include <stout/json.hpp>
#include <stout/net.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>

#include "common/build.hpp"
#include "common/protobuf_utils.hpp"

#include "master/flags.hpp"
#include "master/master.hpp"
#include "master/registry_operations.hpp"

#include "master/allocator/mesos/allocator.hpp"

#include "master/contender/zookeeper.hpp"

#include "master/detector/standalone.hpp"

#include "slave/constants.hpp"
#include "slave/gc.hpp"
#include "slave/gc_process.hpp"
#include "slave/flags.hpp"
#include "slave/slave.hpp"

#include "slave/containerizer/fetcher.hpp"

#include "slave/containerizer/mesos/containerizer.hpp"

#include "tests/containerizer.hpp"
#include "tests/limiter.hpp"
#include "tests/mesos.hpp"
#include "tests/resources_utils.hpp"
#include "tests/utils.hpp"

using google::protobuf::RepeatedPtrField;

using mesos::internal::master::Master;

using mesos::internal::master::allocator::MesosAllocatorProcess;

using mesos::internal::protobuf::createLabel;

using mesos::internal::slave::Containerizer;
using mesos::internal::slave::Fetcher;
using mesos::internal::slave::GarbageCollectorProcess;
using mesos::internal::slave::MesosContainerizer;
using mesos::internal::slave::Slave;

using mesos::master::contender::MASTER_CONTENDER_ZK_SESSION_TIMEOUT;

using mesos::master::detector::MasterDetector;
using mesos::master::detector::StandaloneMasterDetector;

using mesos::v1::scheduler::Call;
using mesos::v1::scheduler::Event;

using process::Clock;
using process::Future;
using process::Message;
using process::Owned;
using process::PID;
using process::Promise;

using process::http::Accepted;
using process::http::OK;
using process::http::Response;
using process::http::Unauthorized;

using std::shared_ptr;
using std::string;
using std::vector;

using testing::_;
using testing::AtMost;
using testing::DoAll;
using testing::Eq;
using testing::Not;
using testing::Return;
using testing::SaveArg;
using testing::WithParamInterface;

namespace mesos {
namespace internal {
namespace tests {

// Those of the overall Mesos master/slave/scheduler/driver tests
// that seem vaguely more master than slave-related are in this file.
// The others are in "slave_tests.cpp".

class MasterTest : public MesosTest {};


TEST_F(MasterTest, TaskRunning)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), &containerizer);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  // Ensure the hostname and url are set correctly.
  EXPECT_EQ(
      slave.get()->pid.address.hostname().get(),
      offers.get()[0].hostname());

  mesos::URL url;
  url.set_scheme("http");
  url.mutable_address()->set_ip(stringify(slave.get()->pid.address.ip));
  url.mutable_address()->set_hostname(
      slave.get()->pid.address.hostname().get());

  url.mutable_address()->set_port(slave.get()->pid.address.port);
  url.set_path("/" + slave.get()->pid.id);

  EXPECT_EQ(url, offers.get()[0].url());

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task.mutable_resources()->MergeFrom(offers.get()[0].resources());
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  EXPECT_CALL(exec, registered(_, _, _, _));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<Nothing> update;
  EXPECT_CALL(containerizer,
              update(_, Resources(offers.get()[0].resources())))
    .WillOnce(DoAll(FutureSatisfy(&update),
                    Return(Nothing())));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status->state());
  EXPECT_TRUE(status->has_executor_id());
  EXPECT_EQ(exec.id, status->executor_id());

  AWAIT_READY(update);

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();
}


// This test ensures that stopping a scheduler driver triggers
// executor's shutdown callback and all still running tasks are
// marked as killed.
TEST_F(MasterTest, ShutdownFrameworkWhileTaskRunning)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  slave::Flags flags = CreateSlaveFlags();

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &containerizer, flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());
  Offer offer = offers.get()[0];

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offer.slave_id());
  task.mutable_resources()->MergeFrom(offer.resources());
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  EXPECT_CALL(exec, registered(_, _, _, _));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<Nothing> update;
  EXPECT_CALL(containerizer,
              update(_, Resources(offer.resources())))
    .WillOnce(DoAll(FutureSatisfy(&update),
                    Return(Nothing())));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.launchTasks(offer.id(), {task});

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status->state());

  AWAIT_READY(update);

  // Set expectation that Master receives teardown call, which
  // triggers marking running tasks as killed.
  Future<mesos::scheduler::Call> teardownCall = FUTURE_CALL(
      mesos::scheduler::Call(), mesos::scheduler::Call::TEARDOWN, _, _);

  // Set expectation that Executor's shutdown callback is invoked.
  Future<Nothing> shutdown;
  EXPECT_CALL(exec, shutdown(_))
    .WillOnce(FutureSatisfy(&shutdown));

  // Stop the driver while the task is running.
  driver.stop();
  driver.join();

  // Wait for teardown call to be dispatched and executor's shutdown
  // callback to be called.
  AWAIT_READY(teardownCall);
  AWAIT_READY(shutdown);

  // We have to be sure the teardown call is processed completely and
  // running tasks enter a terminal state before we request the master
  // state.
  Clock::pause();
  Clock::settle();
  Clock::resume();

  // Request master state.
  Future<Response> response = process::http::get(
      master.get()->pid,
      "state",
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

  // These checks are not essential for the test, but may help
  // understand what went wrong.
  Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
  ASSERT_SOME(parse);

  // Make sure the task landed in completed and marked as killed.
  Result<JSON::String> state = parse->find<JSON::String>(
      "completed_frameworks[0].completed_tasks[0].state");

  ASSERT_SOME_EQ(JSON::String("TASK_KILLED"), state);
}


TEST_F(MasterTest, KillTask)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), &containerizer);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  TaskID taskId;
  taskId.set_value("1");

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->MergeFrom(taskId);
  task.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task.mutable_resources()->MergeFrom(offers.get()[0].resources());
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  EXPECT_CALL(exec, registered(_, _, _, _));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status->state());

  EXPECT_CALL(exec, killTask(_, _))
    .WillOnce(SendStatusUpdateFromTaskID(TASK_KILLED));

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.killTask(taskId);

  AWAIT_READY(status);
  EXPECT_EQ(TASK_KILLED, status->state());

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();
}


// This test ensures that a killTask for an unknown task results in a
// TASK_LOST when there are no slaves in transitionary states.
TEST_F(MasterTest, KillUnknownTask)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), &containerizer);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  TaskID taskId;
  taskId.set_value("1");

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->MergeFrom(taskId);
  task.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task.mutable_resources()->MergeFrom(offers.get()[0].resources());
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  EXPECT_CALL(exec, registered(_, _, _, _));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status->state());

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  TaskID unknownTaskId;
  unknownTaskId.set_value("2");

  driver.killTask(unknownTaskId);

  AWAIT_READY(status);

  EXPECT_EQ(TASK_LOST, status->state());
  EXPECT_EQ(TaskStatus::SOURCE_MASTER, status->source());
  EXPECT_EQ(TaskStatus::REASON_RECONCILIATION, status->reason());

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();
}


TEST_F(MasterTest, KillUnknownTaskSlaveInTransition)
{
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.registry = "replicated_log";

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);
  StandaloneMasterDetector detector(master.get()->pid);

  // Reuse slaveFlags so both StartSlave() use the same work_dir.
  slave::Flags slaveFlags = CreateSlaveFlags();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(&detector, &containerizer, slaveFlags);
  ASSERT_SOME(slave);

  // Wait for slave registration.
  AWAIT_READY(slaveRegisteredMessage);

  MockScheduler sched;
  TestingMesosSchedulerDriver driver(&sched, &detector);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  // Start a task.
  TaskInfo task = createTask(offers.get()[0], "", DEFAULT_EXECUTOR_ID);

  EXPECT_CALL(exec, registered(_, _, _, _));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status->state());

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  // Stop master and slave.
  master->reset();
  slave.get()->terminate();
  slave->reset();

  Future<Nothing> disconnected;
  EXPECT_CALL(sched, disconnected(&driver))
    .WillOnce(FutureSatisfy(&disconnected));

  // Restart master with a mock authorizer to block agent state transitioning.
  MockAuthorizer authorizer;
  master = StartMaster(&authorizer, masterFlags);
  ASSERT_SOME(master);

  frameworkId = Future<FrameworkID>();
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  // Simulate a spurious event (e.g., due to ZooKeeper
  // expiration) at the scheduler.
  detector.appoint(master.get()->pid);

  AWAIT_READY(disconnected);
  AWAIT_READY(frameworkId);

  // Intercept agent authorization.
  Future<Nothing> authorize;
  Promise<bool> promise; // Never satisfied.
  EXPECT_CALL(authorizer, authorized(_))
    .WillOnce(DoAll(FutureSatisfy(&authorize),
                    Return(promise.future())));

  // Restart slave.
  slave = StartSlave(&detector, &containerizer, slaveFlags);
  ASSERT_SOME(slave);

  // Wait for the slave to start reregistration.
  AWAIT_READY(authorize);

  // As Master::killTask isn't doing anything, we shouldn't get a status update.
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .Times(0);

  Future<mesos::scheduler::Call> killCall = FUTURE_CALL(
      mesos::scheduler::Call(), mesos::scheduler::Call::KILL, _, _);

  // Attempt to kill unknown task while slave is transitioning.
  TaskID unknownTaskId;
  unknownTaskId.set_value("2");

  ASSERT_FALSE(unknownTaskId == task.task_id());

  Clock::pause();

  driver.killTask(unknownTaskId);

  AWAIT_READY(killCall);

  // Wait for all messages to be dispatched and processed completely to satisfy
  // the expectation that we didn't receive a status update.
  Clock::settle();

  Clock::resume();

  driver.stop();
  driver.join();
}


// This test checks that the HTTP endpoints return the expected
// information for agents that the master is in the process of marking
// unreachable, but that have not yet been so marked (because the
// registry update hasn't completed yet).
TEST_F(MasterTest, EndpointsForHalfRemovedSlave)
{
  master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Set these expectations up before we spawn the slave so that we
  // don't miss the first PING.
  Future<process::Message> ping = FUTURE_MESSAGE(
      Eq(PingSlaveMessage().GetTypeName()), _, _);

  // Drop all the PONGs to simulate slave partition.
  DROP_PROTOBUFS(PongSlaveMessage(), _, _);

  Clock::pause();

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  slave::Flags agentFlags = CreateSlaveFlags();
  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), agentFlags);
  ASSERT_SOME(slave);

  Clock::advance(agentFlags.registration_backoff_factor);

  AWAIT_READY(slaveRegisteredMessage);

  // Now advance through the PINGs.
  size_t pings = 0;
  while (true) {
    AWAIT_READY(ping);
    pings++;
    if (pings == masterFlags.max_agent_ping_timeouts) {
      break;
    }
    ping = FUTURE_MESSAGE(Eq(PingSlaveMessage().GetTypeName()), _, _);
    Clock::advance(masterFlags.agent_ping_timeout);
  }

  // Intercept the first registrar operation that is attempted; this
  // should be the operation that marks the slave as unreachable.
  Future<Owned<master::Operation>> unreachable;
  Promise<bool> promise;
  EXPECT_CALL(*master.get()->registrar, apply(_))
    .WillOnce(DoAll(FutureArg<0>(&unreachable),
                    Return(promise.future())));

  Clock::advance(masterFlags.agent_ping_timeout);

  slave.get()->terminate();
  slave->reset();

  // Wait for the master to attempt to update the registry, but don't
  // allow the registry update to succeed yet.
  AWAIT_READY(unreachable);
  EXPECT_NE(
      nullptr,
      dynamic_cast<master::MarkSlaveUnreachable*>(unreachable->get()));

  // Settle the clock for the sake of paranoia.
  Clock::settle();

  // Metrics should not be updated yet.
  JSON::Object stats1 = Metrics();
  EXPECT_EQ(1, stats1.values["master/slave_unreachable_scheduled"]);
  EXPECT_EQ(1, stats1.values["master/slave_unreachable_completed"]);
  EXPECT_EQ(0, stats1.values["master/slave_removals"]);
  EXPECT_EQ(0, stats1.values["master/slave_removals/reason_unhealthy"]);
  EXPECT_EQ(0, stats1.values["master/slave_removals/reason_unregistered"]);

  // HTTP endpoints (e.g., /state) should not reflect the removal of
  // the slave yet.
  Future<Response> response1 = process::http::get(
      master.get()->pid,
      "state",
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response1);
  AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response1);

  Try<JSON::Object> parse1 = JSON::parse<JSON::Object>(response1->body);
  Result<JSON::Array> array1 = parse1->find<JSON::Array>("slaves");
  ASSERT_SOME(array1);
  EXPECT_EQ(1u, array1->values.size());

  // Allow the registry operation to return success. Note that we
  // don't actually update the registry here, since the test doesn't
  // require it.
  promise.set(true);

  Clock::settle();

  // Metrics should be updated.
  JSON::Object stats2 = Metrics();
  EXPECT_EQ(1, stats2.values["master/slave_unreachable_scheduled"]);
  EXPECT_EQ(1, stats2.values["master/slave_unreachable_completed"]);
  EXPECT_EQ(1, stats2.values["master/slave_removals"]);
  EXPECT_EQ(1, stats2.values["master/slave_removals/reason_unhealthy"]);
  EXPECT_EQ(0, stats2.values["master/slave_removals/reason_unregistered"]);

  // HTTP endpoints should be updated.
  Future<Response> response2 = process::http::get(
      master.get()->pid,
      "state",
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response2);
  AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response2);

  Try<JSON::Object> parse2 = JSON::parse<JSON::Object>(response2->body);
  Result<JSON::Array> array2 = parse2->find<JSON::Array>("slaves");
  ASSERT_SOME(array2);
  EXPECT_TRUE(array2->values.empty());

  Clock::resume();
}


TEST_F(MasterTest, StatusUpdateAck)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), &containerizer);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task.mutable_resources()->MergeFrom(offers.get()[0].resources());
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  EXPECT_CALL(exec, registered(_, _, _, _));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<StatusUpdateAcknowledgementMessage> acknowledgement =
    FUTURE_PROTOBUF(
        StatusUpdateAcknowledgementMessage(),
        _,
        Eq(slave.get()->pid));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status->state());

  // Ensure the slave gets a status update ACK.
  AWAIT_READY(acknowledgement);

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();
}


// This test checks that domain information is correctly returned by
// the master's HTTP endpoints.
TEST_F(MasterTest, DomainEndpoints)
{
  const string MASTER_REGION = "region-abc";
  const string MASTER_ZONE = "zone-123";

  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.domain = createDomainInfo(MASTER_REGION, MASTER_ZONE);

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  const string AGENT_REGION = "region-xyz";
  const string AGENT_ZONE = "zone-456";

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.domain = createDomainInfo(AGENT_REGION, AGENT_ZONE);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  StandaloneMasterDetector detector(master.get()->pid);
  Try<Owned<cluster::Slave>> slave = StartSlave(&detector, slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);

  // Query the "/state" master endpoint.
  {
    Future<Response> response = process::http::get(
        master.get()->pid,
        "state",
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
    AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
    ASSERT_SOME(parse);

    Result<JSON::String> masterRegion = parse->find<JSON::String>(
        "domain.fault_domain.region.name");
    Result<JSON::String> masterZone = parse->find<JSON::String>(
        "domain.fault_domain.zone.name");

    EXPECT_SOME_EQ(JSON::String(MASTER_REGION), masterRegion);
    EXPECT_SOME_EQ(JSON::String(MASTER_ZONE), masterZone);

    Result<JSON::String> leaderRegion = parse->find<JSON::String>(
        "leader_info.domain.fault_domain.region.name");
    Result<JSON::String> leaderZone = parse->find<JSON::String>(
        "leader_info.domain.fault_domain.zone.name");

    EXPECT_SOME_EQ(JSON::String(MASTER_REGION), leaderRegion);
    EXPECT_SOME_EQ(JSON::String(MASTER_ZONE), leaderZone);

    Result<JSON::String> agentRegion = parse->find<JSON::String>(
        "slaves[0].domain.fault_domain.region.name");
    Result<JSON::String> agentZone = parse->find<JSON::String>(
        "slaves[0].domain.fault_domain.zone.name");

    EXPECT_SOME_EQ(JSON::String(AGENT_REGION), agentRegion);
    EXPECT_SOME_EQ(JSON::String(AGENT_ZONE), agentZone);
  }

  // Query the "/state-summary" master endpoint.
  {
    Future<Response> response = process::http::get(
        master.get()->pid,
        "state-summary",
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
    AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
    ASSERT_SOME(parse);

    Result<JSON::String> agentRegion = parse->find<JSON::String>(
        "slaves[0].domain.fault_domain.region.name");
    Result<JSON::String> agentZone = parse->find<JSON::String>(
        "slaves[0].domain.fault_domain.zone.name");

    EXPECT_SOME_EQ(JSON::String(AGENT_REGION), agentRegion);
    EXPECT_SOME_EQ(JSON::String(AGENT_ZONE), agentZone);
  }

  // Query the "/slaves" master endpoint.
  {
    Future<Response> response = process::http::get(
        master.get()->pid,
        "slaves",
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
    AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
    ASSERT_SOME(parse);

    Result<JSON::String> agentRegion = parse->find<JSON::String>(
        "slaves[0].domain.fault_domain.region.name");
    Result<JSON::String> agentZone = parse->find<JSON::String>(
        "slaves[0].domain.fault_domain.zone.name");

    EXPECT_SOME_EQ(JSON::String(AGENT_REGION), agentRegion);
    EXPECT_SOME_EQ(JSON::String(AGENT_ZONE), agentZone);
  }

  // Query the "/state" agent endpoint.
  {
    Future<Response> response = process::http::get(
        slave.get()->pid,
        "state",
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
    AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
    ASSERT_SOME(parse);

    Result<JSON::String> agentRegion = parse->find<JSON::String>(
        "domain.fault_domain.region.name");
    Result<JSON::String> agentZone = parse->find<JSON::String>(
        "domain.fault_domain.zone.name");

    EXPECT_SOME_EQ(JSON::String(AGENT_REGION), agentRegion);
    EXPECT_SOME_EQ(JSON::String(AGENT_ZONE), agentZone);
  }
}


TEST_F(MasterTest, RecoverResources)
{
  master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  slave::Flags flags = CreateSlaveFlags();
  flags.resources = Option<string>(
      "cpus:2;gpus:0;mem:1024;disk:1024;ports:[1-10, 20-30]");

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &containerizer, flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  ExecutorInfo executorInfo;
  executorInfo.MergeFrom(DEFAULT_EXECUTOR_INFO);

  Resources executorResources = allocatedResources(
      Resources::parse("cpus:0.3;mem:200;ports:[5-8, 23-25]").get(),
      DEFAULT_FRAMEWORK_INFO.roles(0));
  executorInfo.mutable_resources()->MergeFrom(executorResources);

  TaskID taskId;
  taskId.set_value("1");

  Resources taskResources = offers.get()[0].resources() - executorResources;

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->MergeFrom(taskId);
  task.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task.mutable_resources()->MergeFrom(taskResources);
  task.mutable_executor()->MergeFrom(executorInfo);

  EXPECT_CALL(exec, registered(_, _, _, _));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status->state());

  EXPECT_CALL(exec, killTask(_, _))
    .WillOnce(SendStatusUpdateFromTaskID(TASK_KILLED));

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  // Scheduler should get an offer for killed task's resources.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.killTask(taskId);

  AWAIT_READY(status);
  EXPECT_EQ(TASK_KILLED, status->state());

  driver.reviveOffers(); // Don't wait till the next allocation.

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  Offer offer = offers.get()[0];
  EXPECT_EQ(taskResources, offer.resources());

  driver.declineOffer(offer.id());

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  EXPECT_CALL(sched, executorLost(&driver, DEFAULT_EXECUTOR_ID, _, _));

  // Now kill the executor, scheduler should get an offer it's resources.
  containerizer.destroy(offer.framework_id(), executorInfo.executor_id());

  // Ensure the container is destroyed, `ExitedExecutorMessage` message
  // is received by the master and hence its resources will be recovered
  // before a batch allocation is triggered.
  Clock::pause();
  Clock::settle();
  Clock::advance(masterFlags.allocation_interval);
  Clock::resume();

  // TODO(benh): We can't do driver.reviveOffers() because we need to
  // wait for the killed executors resources to get aggregated! We
  // should wait for the allocator to recover the resources first. See
  // the allocator tests for inspiration.

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  Resources slaveResources = Resources::parse(flags.resources.get()).get();
  EXPECT_EQ(allocatedResources(slaveResources, DEFAULT_FRAMEWORK_INFO.roles(0)),
            offers.get()[0].resources());

  driver.stop();
  driver.join();
}


TEST_F(MasterTest, FrameworkMessage)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), &containerizer);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver schedDriver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&schedDriver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&schedDriver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  schedDriver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task.mutable_resources()->MergeFrom(offers.get()[0].resources());
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  Future<ExecutorDriver*> execDriver;
  EXPECT_CALL(exec, registered(_, _, _, _))
    .WillOnce(FutureArg<0>(&execDriver));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&schedDriver, _))
    .WillOnce(FutureArg<1>(&status));

  schedDriver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status->state());

  Future<string> execData;
  EXPECT_CALL(exec, frameworkMessage(_, _))
    .WillOnce(FutureArg<1>(&execData));

  schedDriver.sendFrameworkMessage(
      DEFAULT_EXECUTOR_ID, offers.get()[0].slave_id(), "hello");

  AWAIT_READY(execData);
  EXPECT_EQ("hello", execData.get());

  Future<string> schedData;
  EXPECT_CALL(sched, frameworkMessage(&schedDriver, _, _, _))
    .WillOnce(FutureArg<3>(&schedData));

  execDriver.get()->sendFrameworkMessage("world");

  AWAIT_READY(schedData);
  EXPECT_EQ("world", schedData.get());

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  schedDriver.stop();
  schedDriver.join();
}


TEST_F(MasterTest, MultipleExecutors)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  ExecutorInfo executor1 = createExecutorInfo("executor-1", "exit 1");
  ExecutorInfo executor2 = createExecutorInfo("executor-2", "exit 1");

  MockExecutor exec1(executor1.executor_id());
  MockExecutor exec2(executor2.executor_id());

  hashmap<ExecutorID, Executor*> execs;
  execs[executor1.executor_id()] = &exec1;
  execs[executor2.executor_id()] = &exec2;

  TestContainerizer containerizer(execs);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), &containerizer);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  TaskInfo task1;
  task1.set_name("");
  task1.mutable_task_id()->set_value("1");
  task1.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task1.mutable_resources()->MergeFrom(
      Resources::parse("cpus:1;mem:512").get());
  task1.mutable_executor()->MergeFrom(executor1);

  TaskInfo task2;
  task2.set_name("");
  task2.mutable_task_id()->set_value("2");
  task2.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task2.mutable_resources()->MergeFrom(
      Resources::parse("cpus:1;mem:512").get());
  task2.mutable_executor()->MergeFrom(executor2);

  vector<TaskInfo> tasks;
  tasks.push_back(task1);
  tasks.push_back(task2);

  EXPECT_CALL(exec1, registered(_, _, _, _));

  Future<TaskInfo> exec1Task;
  EXPECT_CALL(exec1, launchTask(_, _))
    .WillOnce(DoAll(SendStatusUpdateFromTask(TASK_RUNNING),
                    FutureArg<1>(&exec1Task)));

  EXPECT_CALL(exec2, registered(_, _, _, _));

  Future<TaskInfo> exec2Task;
  EXPECT_CALL(exec2, launchTask(_, _))
    .WillOnce(DoAll(SendStatusUpdateFromTask(TASK_RUNNING),
                    FutureArg<1>(&exec2Task)));

  Future<TaskStatus> status1, status2;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status1))
    .WillOnce(FutureArg<1>(&status2));

  driver.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_READY(exec1Task);
  EXPECT_EQ(task1.task_id(), exec1Task->task_id());

  AWAIT_READY(exec2Task);
  EXPECT_EQ(task2.task_id(), exec2Task->task_id());

  AWAIT_READY(status1);
  EXPECT_EQ(TASK_RUNNING, status1->state());

  AWAIT_READY(status2);
  EXPECT_EQ(TASK_RUNNING, status2->state());

  EXPECT_CALL(exec1, shutdown(_))
    .Times(AtMost(1));

  EXPECT_CALL(exec2, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();
}


TEST_F(MasterTest, MasterInfo)
{
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.domain = createDomainInfo("region-abc", "zone-xyz");

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<MasterInfo> masterInfo;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<2>(&masterInfo));

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(Return()); // Ignore offers.

  driver.start();

  AWAIT_READY(masterInfo);
  EXPECT_EQ(masterFlags.domain, masterInfo->domain());
  EXPECT_EQ(master.get()->pid.address.port, masterInfo->port());
  EXPECT_EQ(
      master.get()->pid.address.ip,
      net::IP(ntohl(masterInfo->ip())));

  driver.stop();
  driver.join();
}


TEST_F(MasterTest, MasterInfoOnReElection)
{
  master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  StandaloneMasterDetector detector(master.get()->pid);

  Try<Owned<cluster::Slave>> slave = StartSlave(&detector);
  ASSERT_SOME(slave);

  MockScheduler sched;
  TestingMesosSchedulerDriver driver(&sched, &detector);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<Nothing> resourceOffers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureSatisfy(&resourceOffers));

  Future<process::Message> message =
    FUTURE_MESSAGE(Eq(FrameworkRegisteredMessage().GetTypeName()), _, _);

  driver.start();

  AWAIT_READY(message);
  AWAIT_READY(resourceOffers);

  Future<Nothing> disconnected;
  EXPECT_CALL(sched, disconnected(&driver))
    .WillOnce(FutureSatisfy(&disconnected));

  Future<MasterInfo> masterInfo;
  EXPECT_CALL(sched, reregistered(&driver, _))
    .WillOnce(FutureArg<1>(&masterInfo));

  Future<Nothing> resourceOffers2;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureSatisfy(&resourceOffers2))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  // Simulate a spurious event (e.g., due to ZooKeeper
  // expiration) at the scheduler.
  detector.appoint(master.get()->pid);

  AWAIT_READY(disconnected);

  AWAIT_READY(masterInfo);
  EXPECT_EQ(master.get()->pid.address.port, masterInfo->port());
  EXPECT_EQ(
      master.get()->pid.address.ip,
      net::IP(ntohl(masterInfo->ip())));

  EXPECT_EQ(MESOS_VERSION, masterInfo->version());

  // Advance the clock and trigger a batch allocation.
  Clock::pause();
  Clock::advance(masterFlags.allocation_interval);
  Clock::resume();

  // The re-registered framework should get offers.
  AWAIT_READY(resourceOffers2);

  driver.stop();
  driver.join();
}


class WhitelistTest : public MasterTest
{
protected:
  WhitelistTest()
    : path("whitelist.txt")
  {}

  virtual ~WhitelistTest()
  {
    os::rm(path);
  }
  const string path;
};


TEST_F(WhitelistTest, WhitelistSlave)
{
  // Add some hosts to the white list.
  Try<string> hostname = net::hostname();
  ASSERT_SOME(hostname);

  string hosts = hostname.get() + "\n" + "dummy-agent";
  ASSERT_SOME(os::write(path, hosts)) << "Error writing whitelist";

  master::Flags flags = CreateMasterFlags();
  flags.whitelist = path;

  Try<Owned<cluster::Master>> master = StartMaster(flags);
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.hostname = hostname.get();

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  AWAIT_READY(offers); // Implies the slave has registered.

  driver.stop();
  driver.join();
}


class HostnameTest : public MasterTest {};


TEST_F(HostnameTest, LookupEnabled)
{
  master::Flags flags = CreateMasterFlags();
  EXPECT_TRUE(flags.hostname_lookup);

  Try<Owned<cluster::Master>> master = StartMaster(flags);
  ASSERT_SOME(master);

  EXPECT_EQ(
      master.get()->pid.address.hostname().get(),
      master.get()->getMasterInfo().hostname());
}


TEST_F(HostnameTest, LookupDisabled)
{
  master::Flags flags = CreateMasterFlags();
  EXPECT_TRUE(flags.hostname_lookup);
  EXPECT_NONE(flags.hostname);

  flags.hostname_lookup = false;

  Try<Owned<cluster::Master>> master = StartMaster(flags);
  ASSERT_SOME(master);

  EXPECT_EQ(
      stringify(master.get()->pid.address.ip),
      master.get()->getMasterInfo().hostname());
}


TEST_F(MasterTest, MasterLost)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  StandaloneMasterDetector detector(master.get()->pid);

  Try<Owned<cluster::Slave>> slave = StartSlave(&detector);
  ASSERT_SOME(slave);

  MockScheduler sched;
  TestingMesosSchedulerDriver driver(&sched, &detector);

  EXPECT_CALL(sched, registered(&driver, _, _));

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(Return()); // Ignore offers.

  Future<process::Message> message =
    FUTURE_MESSAGE(Eq(FrameworkRegisteredMessage().GetTypeName()), _, _);

  driver.start();

  AWAIT_READY(message);

  Future<Nothing> disconnected;
  EXPECT_CALL(sched, disconnected(&driver))
    .WillOnce(FutureSatisfy(&disconnected));

  // Simulate a spurious event at the scheduler.
  detector.appoint(None());

  AWAIT_READY(disconnected);

  driver.stop();
  driver.join();
}


// Test ensures two offers from same slave can be used for single task.
// This is done by first launching single task which utilize half of the
// available resources. A subsequent offer for the rest of the available
// resources will be sent by master. The first task is killed and an offer
// for the remaining resources will be sent. Which means two offers covering
// all slave resources and a single task should be able to run on these.
TEST_F(MasterTest, LaunchCombinedOfferTest)
{
  master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  // The CPU granularity is 1.0 which means that we need slaves with at least
  // 2 cpus for a combined offer.
  Resources halfSlave = Resources::parse("cpus:1;mem:512").get();
  Resources fullSlave = halfSlave + halfSlave;

  slave::Flags flags = CreateSlaveFlags();
  flags.resources = Option<string>(stringify(fullSlave));

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &containerizer, flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  // Get 1st offer and use half of the slave resources to get subsequent offer.
  Future<vector<Offer>> offers1;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers1));

  driver.start();

  AWAIT_READY(offers1);
  ASSERT_FALSE(offers1->empty());
  Resources resources1(offers1.get()[0].resources());
  EXPECT_EQ(2, resources1.cpus().get());
  EXPECT_EQ(Megabytes(1024), resources1.mem().get());

  TaskInfo task1;
  task1.set_name("");
  task1.mutable_task_id()->set_value("1");
  task1.mutable_slave_id()->MergeFrom(offers1.get()[0].slave_id());
  task1.mutable_resources()->MergeFrom(halfSlave);
  task1.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  EXPECT_CALL(exec, registered(_, _, _, _));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> status1;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status1));

  Future<vector<Offer>> offers2;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers2));

  // We want to be notified immediately with new offer.
  Filters filters;
  filters.set_refuse_seconds(0);

  driver.launchTasks(offers1.get()[0].id(), {task1}, filters);

  AWAIT_READY(status1);
  EXPECT_EQ(TASK_RUNNING, status1->state());

  // Advance the clock and trigger a batch allocation.
  Clock::pause();
  Clock::advance(masterFlags.allocation_interval);
  Clock::resume();

  // Await 2nd offer.
  AWAIT_READY(offers2);
  ASSERT_FALSE(offers2->empty());

  Resources resources2(offers2.get()[0].resources());
  EXPECT_EQ(1, resources2.cpus().get());
  EXPECT_EQ(Megabytes(512), resources2.mem().get());

  Future<TaskStatus> status2;
  EXPECT_CALL(exec, killTask(_, _))
    .WillOnce(SendStatusUpdateFromTaskID(TASK_KILLED));

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status2));

  Future<vector<Offer>> offers3;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers3))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  // Kill 1st task.
  TaskID taskId1 = task1.task_id();
  driver.killTask(taskId1);

  AWAIT_READY(status2);
  EXPECT_EQ(TASK_KILLED, status2->state());

  // Advance the clock and trigger a batch allocation.
  Clock::pause();
  Clock::advance(masterFlags.allocation_interval);
  Clock::resume();

  // Await 3rd offer - 2nd and 3rd offer to same slave are now ready.
  AWAIT_READY(offers3);
  ASSERT_FALSE(offers3->empty());
  Resources resources3(offers3.get()[0].resources());
  EXPECT_EQ(1, resources3.cpus().get());
  EXPECT_EQ(Megabytes(512), resources3.mem().get());

  TaskInfo task2;
  task2.set_name("");
  task2.mutable_task_id()->set_value("2");
  task2.mutable_slave_id()->MergeFrom(offers2.get()[0].slave_id());
  task2.mutable_resources()->MergeFrom(fullSlave);
  task2.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> status3;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status3));

  vector<OfferID> combinedOffers;
  combinedOffers.push_back(offers2.get()[0].id());
  combinedOffers.push_back(offers3.get()[0].id());

  driver.launchTasks(combinedOffers, {task2});

  AWAIT_READY(status3);
  EXPECT_EQ(TASK_RUNNING, status3->state());

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();
}


// This test ensures that the offers provided to a single launchTasks
// call cannot span multiple slaves. A non-partition-aware framework
// should receive TASK_LOST.
TEST_F(MasterTest, LaunchAcrossSlavesLost)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  // See LaunchCombinedOfferTest() for resource size motivation.
  Resources fullSlave = Resources::parse("cpus:2;mem:1024").get();
  Resources twoSlaves = fullSlave + fullSlave;

  slave::Flags flags = CreateSlaveFlags();
  flags.resources = Option<string>(stringify(fullSlave));

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave1 =
    StartSlave(detector.get(), &containerizer, flags);
  ASSERT_SOME(slave1);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers1;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers1));

  driver.start();

  AWAIT_READY(offers1);
  ASSERT_FALSE(offers1->empty());
  Resources resources1(offers1.get()[0].resources());
  EXPECT_EQ(2, resources1.cpus().get());
  EXPECT_EQ(Megabytes(1024), resources1.mem().get());

  // Test that offers cannot span multiple slaves.
  Future<vector<Offer>> offers2;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers2))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  // Create new Flags as we require another work_dir for checkpoints.
  slave::Flags flags2 = CreateSlaveFlags();
  flags2.resources = Option<string>(stringify(fullSlave));

  Try<Owned<cluster::Slave>> slave2 =
    StartSlave(detector.get(), &containerizer, flags2);
  ASSERT_SOME(slave2);

  AWAIT_READY(offers2);
  ASSERT_FALSE(offers2->empty());
  Resources resources2(offers1.get()[0].resources());
  EXPECT_EQ(2, resources2.cpus().get());
  EXPECT_EQ(Megabytes(1024), resources2.mem().get());

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers1.get()[0].slave_id());
  task.mutable_resources()->MergeFrom(twoSlaves);
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  vector<OfferID> combinedOffers;
  combinedOffers.push_back(offers1.get()[0].id());
  combinedOffers.push_back(offers2.get()[0].id());

  Future<Nothing> recoverResources =
    FUTURE_DISPATCH(_, &MesosAllocatorProcess::recoverResources);

  driver.launchTasks(combinedOffers, {task});

  AWAIT_READY(status);
  EXPECT_EQ(TASK_LOST, status->state());
  EXPECT_EQ(TaskStatus::REASON_INVALID_OFFERS, status->reason());

  // The resources of the invalid offers should be recovered.
  AWAIT_READY(recoverResources);

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  // Check metrics.
  JSON::Object stats = Metrics();
  EXPECT_EQ(0u, stats.values["master/tasks_dropped"]);
  EXPECT_EQ(1u, stats.values["master/tasks_lost"]);
  EXPECT_EQ(
      1u,
      stats.values["master/task_lost/source_master/reason_invalid_offers"]);

  driver.stop();
  driver.join();
}


// This test ensures that the offers provided to a single launchTasks
// call cannot span multiple slaves. A partition-aware framework
// should receive TASK_DROPPED.
TEST_F(MasterTest, LaunchAcrossSlavesDropped)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  // See LaunchCombinedOfferTest() for resource size motivation.
  Resources fullSlave = Resources::parse("cpus:2;mem:1024").get();
  Resources twoSlaves = fullSlave + fullSlave;

  slave::Flags flags = CreateSlaveFlags();
  flags.resources = Option<string>(stringify(fullSlave));

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave1 =
    StartSlave(detector.get(), &containerizer, flags);
  ASSERT_SOME(slave1);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.add_capabilities()->set_type(
      FrameworkInfo::Capability::PARTITION_AWARE);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers1;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers1));

  driver.start();

  AWAIT_READY(offers1);
  ASSERT_FALSE(offers1->empty());
  Resources resources1(offers1.get()[0].resources());
  EXPECT_EQ(2, resources1.cpus().get());
  EXPECT_EQ(Megabytes(1024), resources1.mem().get());

  // Test that offers cannot span multiple slaves.
  Future<vector<Offer>> offers2;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers2))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  // Create new Flags as we require another work_dir for checkpoints.
  slave::Flags flags2 = CreateSlaveFlags();
  flags2.resources = Option<string>(stringify(fullSlave));

  Try<Owned<cluster::Slave>> slave2 =
    StartSlave(detector.get(), &containerizer, flags2);
  ASSERT_SOME(slave2);

  AWAIT_READY(offers2);
  ASSERT_FALSE(offers2->empty());
  Resources resources2(offers1.get()[0].resources());
  EXPECT_EQ(2, resources2.cpus().get());
  EXPECT_EQ(Megabytes(1024), resources2.mem().get());

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers1.get()[0].slave_id());
  task.mutable_resources()->MergeFrom(twoSlaves);
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  vector<OfferID> combinedOffers;
  combinedOffers.push_back(offers1.get()[0].id());
  combinedOffers.push_back(offers2.get()[0].id());

  Future<Nothing> recoverResources =
    FUTURE_DISPATCH(_, &MesosAllocatorProcess::recoverResources);

  driver.launchTasks(combinedOffers, {task});

  AWAIT_READY(status);
  EXPECT_EQ(TASK_DROPPED, status->state());
  EXPECT_EQ(TaskStatus::REASON_INVALID_OFFERS, status->reason());

  // The resources of the invalid offers should be recovered.
  AWAIT_READY(recoverResources);

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  // Check metrics.
  JSON::Object stats = Metrics();
  EXPECT_EQ(1u, stats.values.count("master/tasks_dropped"));
  EXPECT_EQ(1u, stats.values["master/tasks_dropped"]);
  EXPECT_EQ(
      1u,
      stats.values.count(
          "master/task_dropped/source_master/reason_invalid_offers"));
  EXPECT_EQ(
      1u,
      stats.values["master/task_dropped/source_master/reason_invalid_offers"]);

  driver.stop();
  driver.join();
}


// This test ensures that an offer cannot appear more than once in the
// offers provided to a single launchTasks call. A non-partition-aware
// framework should receive TASK_LOST.
TEST_F(MasterTest, LaunchDuplicateOfferLost)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  // See LaunchCombinedOfferTest() for resource size motivation.
  Resources fullSlave = Resources::parse("cpus:2;mem:1024").get();

  slave::Flags flags = CreateSlaveFlags();
  flags.resources = Option<string>(stringify(fullSlave));

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &containerizer, flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  // Test that same offers cannot be used more than once.
  // Kill 2nd task and get offer for full slave.
  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());
  Resources resources(offers.get()[0].resources());
  EXPECT_EQ(2, resources.cpus().get());
  EXPECT_EQ(Megabytes(1024), resources.mem().get());

  vector<OfferID> combinedOffers;
  combinedOffers.push_back(offers.get()[0].id());
  combinedOffers.push_back(offers.get()[0].id());

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task.mutable_resources()->MergeFrom(fullSlave);
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  Future<TaskStatus> status;

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  Future<Nothing> recoverResources =
    FUTURE_DISPATCH(_, &MesosAllocatorProcess::recoverResources);

  driver.launchTasks(combinedOffers, {task});

  AWAIT_READY(status);
  EXPECT_EQ(TASK_LOST, status->state());
  EXPECT_EQ(TaskStatus::REASON_INVALID_OFFERS, status->reason());

  // The resources of the invalid offers should be recovered.
  AWAIT_READY(recoverResources);

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  // Check metrics.
  JSON::Object stats = Metrics();
  EXPECT_EQ(0u, stats.values["master/tasks_dropped"]);
  EXPECT_EQ(1u, stats.values["master/tasks_lost"]);
  EXPECT_EQ(
      1u,
      stats.values["master/task_lost/source_master/reason_invalid_offers"]);

  driver.stop();
  driver.join();
}


// This test ensures that an offer cannot appear more than once in the
// offers provided to a single launchTasks call. A partition-aware
// framework should receive TASK_DROPPED.
TEST_F(MasterTest, LaunchDuplicateOfferDropped)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  // See LaunchCombinedOfferTest() for resource size motivation.
  Resources fullSlave = Resources::parse("cpus:2;mem:1024").get();

  slave::Flags flags = CreateSlaveFlags();
  flags.resources = Option<string>(stringify(fullSlave));

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &containerizer, flags);
  ASSERT_SOME(slave);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.add_capabilities()->set_type(
      FrameworkInfo::Capability::PARTITION_AWARE);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  // Test that same offers cannot be used more than once.
  // Kill 2nd task and get offer for full slave.
  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());
  Resources resources(offers.get()[0].resources());
  EXPECT_EQ(2, resources.cpus().get());
  EXPECT_EQ(Megabytes(1024), resources.mem().get());

  vector<OfferID> combinedOffers;
  combinedOffers.push_back(offers.get()[0].id());
  combinedOffers.push_back(offers.get()[0].id());

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task.mutable_resources()->MergeFrom(fullSlave);
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  Future<TaskStatus> status;

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  Future<Nothing> recoverResources =
    FUTURE_DISPATCH(_, &MesosAllocatorProcess::recoverResources);

  driver.launchTasks(combinedOffers, {task});

  AWAIT_READY(status);
  EXPECT_EQ(TASK_DROPPED, status->state());
  EXPECT_EQ(TaskStatus::REASON_INVALID_OFFERS, status->reason());

  // The resources of the invalid offers should be recovered.
  AWAIT_READY(recoverResources);

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  // Check metrics.
  JSON::Object stats = Metrics();
  EXPECT_EQ(0u, stats.values["master/tasks_lost"]);
  EXPECT_EQ(1u, stats.values["master/tasks_dropped"]);
  EXPECT_EQ(
      1u,
      stats.values["master/task_dropped/source_master/reason_invalid_offers"]);

  driver.stop();
  driver.join();
}


// This test ensures that a multi-role framework cannot launch tasks with
// offers allocated to different roles of that framework in a single
// launchTasks call. We follow similar pattern in LaunchCombinedOfferTest.
//
// We launch a cluster with one master and one slave, and a framework
// with two roles. Firstly, total resources will be offered to one of
// the roles (we don't assume that it is deterministic as to which of
// the two roles are chosen first). We launch a task using half of the
// total resources. The other half will be returned to master and offered
// to the other role, since it has a lower share (0). Then we kill the
// task, half of resources will be offered to first role again, since
// the first has a lower share (0). At this point, two offers with
// different roles are outstanding and we can combine them in one
// `launchTasks` call. A non-partition-aware framework should
// receive TASK_LOST.
//
// TODO(jay_guo): Add tests for other operations as well.
TEST_F(MasterTest, LaunchDifferentRoleLost)
{
  Clock::pause();

  master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  // The CPU granularity is 1.0 which means that we need slaves
  // with at least 2 cpus for a combined offer.
  Resources halfSlave = Resources::parse("cpus:1;mem:512").get();
  Resources fullSlave = halfSlave + halfSlave;

  slave::Flags flags = CreateSlaveFlags();
  flags.resources = Option<string>(stringify(fullSlave));

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &containerizer, flags);
  ASSERT_SOME(slave);

  FrameworkInfo framework = DEFAULT_FRAMEWORK_INFO;
  framework.set_roles(0, "role1");
  framework.add_roles("role2");

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, framework, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  // Get 1st offer and use half of the resources.
  Future<vector<Offer>> offers1;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers1));

  driver.start();

  Clock::settle();
  Clock::advance(masterFlags.allocation_interval);

  AWAIT_READY(offers1);
  ASSERT_FALSE(offers1->empty());
  Resources resources1(offers1.get()[0].resources());
  EXPECT_EQ(2, resources1.cpus().get());
  EXPECT_EQ(Megabytes(1024), resources1.mem().get());

  TaskInfo task1;
  task1.set_name("");
  task1.mutable_task_id()->set_value("1");
  task1.mutable_slave_id()->MergeFrom(offers1.get()[0].slave_id());
  task1.mutable_resources()->MergeFrom(halfSlave);
  task1.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  EXPECT_CALL(exec, registered(_, _, _, _));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> status1;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status1));

  Future<vector<Offer>> offers2;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers2));

  // We want to be receive an offer for the remainder immediately.
  Filters filters;
  filters.set_refuse_seconds(0);

  driver.launchTasks(offers1.get()[0].id(), {task1}, filters);

  AWAIT_READY(status1);
  EXPECT_EQ(TASK_RUNNING, status1->state());

  // Advance the clock and trigger a batch allocation.
  Clock::settle();
  Clock::advance(masterFlags.allocation_interval);

  // Await 2nd offer.
  AWAIT_READY(offers2);
  ASSERT_FALSE(offers2->empty());
  ASSERT_TRUE(offers2.get()[0].has_allocation_info());

  Resources resources2(offers2.get()[0].resources());
  EXPECT_EQ(1, resources2.cpus().get());
  EXPECT_EQ(Megabytes(512), resources2.mem().get());

  Future<TaskStatus> status2;
  EXPECT_CALL(exec, killTask(_, _))
    .WillOnce(SendStatusUpdateFromTaskID(TASK_KILLED));

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status2));

  Future<vector<Offer>> offers3;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers3))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  // Kill 1st task.
  TaskID taskId1 = task1.task_id();
  driver.killTask(taskId1);

  AWAIT_READY(status2);
  EXPECT_EQ(TASK_KILLED, status2->state());

  // Advance the clock and trigger a batch allocation.
  Clock::settle();
  Clock::advance(masterFlags.allocation_interval);

  // Await 3rd offer - 2nd and 3rd offer to same slave are now ready.
  AWAIT_READY(offers3);
  ASSERT_FALSE(offers3->empty());
  ASSERT_TRUE(offers3.get()[0].has_allocation_info());
  Resources resources3(offers3.get()[0].resources());
  EXPECT_EQ(1, resources3.cpus().get());
  EXPECT_EQ(Megabytes(512), resources3.mem().get());

  // 2nd and 3rd offer should be allocated to different roles.
  ASSERT_NE(
      offers2.get()[0].allocation_info().role(),
      offers3.get()[0].allocation_info().role());

  TaskInfo task2;
  task2.set_name("");
  task2.mutable_task_id()->set_value("2");
  task2.mutable_slave_id()->MergeFrom(offers2.get()[0].slave_id());
  task2.mutable_resources()->MergeFrom(fullSlave);
  task2.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  Future<TaskStatus> status3;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status3));

  vector<OfferID> combinedOffers;
  combinedOffers.push_back(offers2.get()[0].id());
  combinedOffers.push_back(offers3.get()[0].id());

  Future<Nothing> recoverResources =
    FUTURE_DISPATCH(_, &MesosAllocatorProcess::recoverResources);

  driver.launchTasks(combinedOffers, {task2});

  Clock::settle();

  AWAIT_READY(status3);
  EXPECT_EQ(TASK_LOST, status3->state());
  EXPECT_EQ(TaskStatus::REASON_INVALID_OFFERS, status3->reason());

  // The resources of the invalid offers should be recovered.
  AWAIT_READY(recoverResources);

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();
}


// TODO(vinod): These tests only verify that the master metrics exist
// but we need tests that verify that these metrics are updated.
TEST_F(MasterTest, MetricsInMetricsEndpoint)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  JSON::Object snapshot = Metrics();

  EXPECT_EQ(1u, snapshot.values.count("master/uptime_secs"));

  EXPECT_EQ(1u, snapshot.values.count("master/elected"));
  EXPECT_EQ(1, snapshot.values["master/elected"]);

  EXPECT_EQ(1u, snapshot.values.count("master/slaves_connected"));
  EXPECT_EQ(1u, snapshot.values.count("master/slaves_disconnected"));
  EXPECT_EQ(1u, snapshot.values.count("master/slaves_active"));
  EXPECT_EQ(1u, snapshot.values.count("master/slaves_inactive"));
  EXPECT_EQ(1u, snapshot.values.count("master/slaves_unreachable"));

  EXPECT_EQ(1u, snapshot.values.count("master/frameworks_connected"));
  EXPECT_EQ(1u, snapshot.values.count("master/frameworks_disconnected"));
  EXPECT_EQ(1u, snapshot.values.count("master/frameworks_active"));
  EXPECT_EQ(1u, snapshot.values.count("master/frameworks_inactive"));

  EXPECT_EQ(1u, snapshot.values.count("master/outstanding_offers"));

  EXPECT_EQ(1u, snapshot.values.count("master/tasks_staging"));
  EXPECT_EQ(1u, snapshot.values.count("master/tasks_starting"));
  EXPECT_EQ(1u, snapshot.values.count("master/tasks_running"));
  EXPECT_EQ(1u, snapshot.values.count("master/tasks_unreachable"));
  EXPECT_EQ(1u, snapshot.values.count("master/tasks_killing"));
  EXPECT_EQ(1u, snapshot.values.count("master/tasks_finished"));
  EXPECT_EQ(1u, snapshot.values.count("master/tasks_failed"));
  EXPECT_EQ(1u, snapshot.values.count("master/tasks_killed"));
  EXPECT_EQ(1u, snapshot.values.count("master/tasks_lost"));
  EXPECT_EQ(1u, snapshot.values.count("master/tasks_error"));
  EXPECT_EQ(1u, snapshot.values.count("master/tasks_dropped"));
  EXPECT_EQ(1u, snapshot.values.count("master/tasks_gone"));
  EXPECT_EQ(1u, snapshot.values.count("master/tasks_gone_by_operator"));

  EXPECT_EQ(1u, snapshot.values.count("master/dropped_messages"));

  // Messages from schedulers.
  EXPECT_EQ(1u, snapshot.values.count("master/messages_register_framework"));
  EXPECT_EQ(1u, snapshot.values.count("master/messages_reregister_framework"));
  EXPECT_EQ(1u, snapshot.values.count("master/messages_unregister_framework"));
  EXPECT_EQ(1u, snapshot.values.count("master/messages_deactivate_framework"));
  EXPECT_EQ(1u, snapshot.values.count("master/messages_kill_task"));
  EXPECT_EQ(1u, snapshot.values.count(
      "master/messages_status_update_acknowledgement"));
  EXPECT_EQ(1u, snapshot.values.count("master/messages_resource_request"));
  EXPECT_EQ(1u, snapshot.values.count("master/messages_launch_tasks"));
  EXPECT_EQ(1u, snapshot.values.count("master/messages_decline_offers"));
  EXPECT_EQ(1u, snapshot.values.count("master/messages_revive_offers"));
  EXPECT_EQ(1u, snapshot.values.count("master/messages_suppress_offers"));
  EXPECT_EQ(1u, snapshot.values.count("master/messages_reconcile_tasks"));
  EXPECT_EQ(1u, snapshot.values.count("master/messages_framework_to_executor"));

  // Messages from executors.
  EXPECT_EQ(1u, snapshot.values.count("master/messages_executor_to_framework"));

  // Messages from slaves.
  EXPECT_EQ(1u, snapshot.values.count("master/messages_register_slave"));
  EXPECT_EQ(1u, snapshot.values.count("master/messages_reregister_slave"));
  EXPECT_EQ(1u, snapshot.values.count("master/messages_unregister_slave"));
  EXPECT_EQ(1u, snapshot.values.count("master/messages_status_update"));
  EXPECT_EQ(1u, snapshot.values.count("master/messages_exited_executor"));
  EXPECT_EQ(1u, snapshot.values.count("master/messages_update_slave"));

  // Messages from both schedulers and slaves.
  EXPECT_EQ(1u, snapshot.values.count("master/messages_authenticate"));

  EXPECT_EQ(1u, snapshot.values.count(
      "master/valid_framework_to_executor_messages"));
  EXPECT_EQ(1u, snapshot.values.count(
      "master/invalid_framework_to_executor_messages"));
  EXPECT_EQ(1u, snapshot.values.count(
      "master/valid_executor_to_framework_messages"));
  EXPECT_EQ(1u, snapshot.values.count(
      "master/invalid_executor_to_framework_messages"));

  EXPECT_EQ(1u, snapshot.values.count("master/valid_status_updates"));
  EXPECT_EQ(1u, snapshot.values.count("master/invalid_status_updates"));

  EXPECT_EQ(1u, snapshot.values.count(
      "master/valid_status_update_acknowledgements"));
  EXPECT_EQ(1u, snapshot.values.count(
      "master/invalid_status_update_acknowledgements"));

  // Recovery counters.
  EXPECT_EQ(1u, snapshot.values.count("master/recovery_slave_removals"));

  // Process metrics.
  EXPECT_EQ(1u, snapshot.values.count("master/event_queue_messages"));
  EXPECT_EQ(1u, snapshot.values.count("master/event_queue_dispatches"));
  EXPECT_EQ(1u, snapshot.values.count("master/event_queue_http_requests"));

  // Slave observer metrics.
  EXPECT_EQ(1u, snapshot.values.count("master/slave_unreachable_scheduled"));
  EXPECT_EQ(1u, snapshot.values.count("master/slave_unreachable_completed"));
  EXPECT_EQ(1u, snapshot.values.count("master/slave_unreachable_canceled"));

  EXPECT_EQ(1u, snapshot.values.count("master/cpus_total"));
  EXPECT_EQ(1u, snapshot.values.count("master/cpus_used"));
  EXPECT_EQ(1u, snapshot.values.count("master/cpus_percent"));

  EXPECT_EQ(1u, snapshot.values.count("master/cpus_revocable_total"));
  EXPECT_EQ(1u, snapshot.values.count("master/cpus_revocable_used"));
  EXPECT_EQ(1u, snapshot.values.count("master/cpus_revocable_percent"));

  EXPECT_EQ(1u, snapshot.values.count("master/gpus_total"));
  EXPECT_EQ(1u, snapshot.values.count("master/gpus_used"));
  EXPECT_EQ(1u, snapshot.values.count("master/gpus_percent"));

  EXPECT_EQ(1u, snapshot.values.count("master/gpus_revocable_total"));
  EXPECT_EQ(1u, snapshot.values.count("master/gpus_revocable_used"));
  EXPECT_EQ(1u, snapshot.values.count("master/gpus_revocable_percent"));

  EXPECT_EQ(1u, snapshot.values.count("master/mem_total"));
  EXPECT_EQ(1u, snapshot.values.count("master/mem_used"));
  EXPECT_EQ(1u, snapshot.values.count("master/mem_percent"));

  EXPECT_EQ(1u, snapshot.values.count("master/mem_revocable_total"));
  EXPECT_EQ(1u, snapshot.values.count("master/mem_revocable_used"));
  EXPECT_EQ(1u, snapshot.values.count("master/mem_revocable_percent"));

  EXPECT_EQ(1u, snapshot.values.count("master/disk_total"));
  EXPECT_EQ(1u, snapshot.values.count("master/disk_used"));
  EXPECT_EQ(1u, snapshot.values.count("master/disk_percent"));

  EXPECT_EQ(1u, snapshot.values.count("master/disk_revocable_total"));
  EXPECT_EQ(1u, snapshot.values.count("master/disk_revocable_used"));
  EXPECT_EQ(1u, snapshot.values.count("master/disk_revocable_percent"));

  // Registrar Metrics.
  EXPECT_EQ(1u, snapshot.values.count("registrar/queued_operations"));
  EXPECT_EQ(1u, snapshot.values.count("registrar/registry_size_bytes"));

  EXPECT_EQ(1u, snapshot.values.count("registrar/state_fetch_ms"));
  EXPECT_EQ(1u, snapshot.values.count("registrar/state_store_ms"));

  // Allocator Metrics.
  EXPECT_EQ(1u, snapshot.values.count(
      "allocator/event_queue_dispatches"));
  EXPECT_EQ(1u, snapshot.values.count(
      "allocator/mesos/event_queue_dispatches"));
}


// Ensures that an empty response arrives if information about
// registered slaves is requested from a master where no slaves
// have been registered.
TEST_F(MasterTest, SlavesEndpointWithoutSlaves)
{
  // Start up.
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Query the master.
  Future<Response> response = process::http::get(
      master.get()->pid,
      "slaves",
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

  const Try<JSON::Value> parse = JSON::parse(response->body);
  ASSERT_SOME(parse);

  Try<JSON::Value> expected = JSON::parse(
      "{"
      "  \"slaves\" : [],"
      "  \"recovered_slaves\" : []"
      "}");

  ASSERT_SOME(expected);
  EXPECT_SOME_EQ(expected.get(), parse);
}


// Tests that reservations can only be seen by authorized users.
TEST_F(MasterTest, SlavesEndpointFiltering)
{
  // Start up the master.
  master::Flags flags = CreateMasterFlags();

  {
    mesos::ACL::ViewRole* acl = flags.acls.get().add_view_roles();
    acl->mutable_principals()->add_values(DEFAULT_CREDENTIAL_2.principal());
    acl->mutable_roles()->set_type(mesos::ACL::Entity::NONE);
  }

  Try<Owned<cluster::Master>> master = StartMaster(flags);
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  Future<SlaveRegisteredMessage> agentRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), master.get()->pid, _);

  Try<Owned<cluster::Slave>> agent = StartSlave(detector.get());
  ASSERT_SOME(agent);

  AWAIT_READY(agentRegisteredMessage);
  const SlaveID& agentId = agentRegisteredMessage->slave_id();

  // Create reservation.
  {
    RepeatedPtrField<Resource> reservation =
      Resources::parse("cpus:1;mem:12")->pushReservation(
          createDynamicReservationInfo(
              "superhero",
              DEFAULT_CREDENTIAL.principal()));

    Future<Response> response = process::http::post(
        master.get()->pid,
        "reserve",
        createBasicAuthHeaders(DEFAULT_CREDENTIAL),
        strings::format(
            "slaveId=%s&resources=%s",
            agentId,
            JSON::protobuf(reservation)).get());

    AWAIT_READY(response);
    AWAIT_EXPECT_RESPONSE_STATUS_EQ(Accepted().status, response);
  }

  // Query master with invalid user.
  {
    Future<Response> response = process::http::get(
        master.get()->pid,
        "slaves",
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL_2));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
    AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

    const Try<JSON::Object> json = JSON::parse<JSON::Object>(response->body);
    ASSERT_SOME(json);

    Result<JSON::Object> reservations =
      json->find<JSON::Object>("slaves[0].reserved_resources");
    ASSERT_SOME(reservations);
    EXPECT_TRUE(reservations->values.empty());
  }

  // Query master with valid user.
  {
    Future<Response> response = process::http::get(
        master.get()->pid,
        "slaves",
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
    AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

    const Try<JSON::Object> json = JSON::parse<JSON::Object>(response->body);
    ASSERT_SOME(json);

    Result<JSON::Object> reservations =
      json->find<JSON::Object>("slaves[0].reserved_resources");
    ASSERT_SOME(reservations);
    EXPECT_FALSE(reservations->values.empty());
  }
}


// Ensures that the number of registered slaves reported by
// /master/slaves coincides with the actual number of registered
// slaves.
TEST_F(MasterTest, SlavesEndpointTwoSlaves)
{
  // Start up the master.
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  // Start a couple of slaves. Their only use is for them to register
  // to the master.
  Future<SlaveRegisteredMessage> slave1RegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), master.get()->pid, _);

  Try<Owned<cluster::Slave>> slave1 = StartSlave(detector.get());
  ASSERT_SOME(slave1);

  AWAIT_READY(slave1RegisteredMessage);

  Future<SlaveRegisteredMessage> slave2RegisteredMessage =
    FUTURE_PROTOBUF(
        SlaveRegisteredMessage(), master.get()->pid, Not(slave1.get()->pid));

  Try<Owned<cluster::Slave>> slave2 = StartSlave(detector.get());
  ASSERT_SOME(slave2);

  AWAIT_READY(slave2RegisteredMessage);

  // Query the master.
  Future<Response> response = process::http::get(
      master.get()->pid,
      "slaves",
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

  const Try<JSON::Object> parse =
    JSON::parse<JSON::Object>(response->body);

  ASSERT_SOME(parse);

  // Check that there are two elements in the array.
  Result<JSON::Array> array = parse->find<JSON::Array>("slaves");
  ASSERT_SOME(array);
  EXPECT_EQ(2u, array->values.size());
}


// Ensures that the '/slaves' endpoint returns the correct slave and it's in
// the correct field of the response when provided with a slave ID query
// parameter.
TEST_F(MasterTest, SlavesEndpointQuerySlave)
{
  master::Flags masterFlags = CreateMasterFlags();

  // Ensure that master can recover from the same work_dir.
  masterFlags.registry = "replicated_log";
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  // Start two agents.

  Future<SlaveRegisteredMessage> slave1RegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), master.get()->pid, _);

  Try<Owned<cluster::Slave>> slave1 = StartSlave(detector.get());
  ASSERT_SOME(slave1);

  AWAIT_READY(slave1RegisteredMessage);

  Future<SlaveRegisteredMessage> slave2RegisteredMessage =
    FUTURE_PROTOBUF(
        SlaveRegisteredMessage(),
        master.get()->pid,
        Not(slave1.get()->pid));

  Try<Owned<cluster::Slave>> slave2 = StartSlave(detector.get());
  ASSERT_SOME(slave2);

  AWAIT_READY(slave2RegisteredMessage);

  // Query the information about the first agent.
  {
    string slaveId = slave1RegisteredMessage->slave_id().value();

    Future<Response> response = process::http::get(
        master.get()->pid,
        "slaves?slave_id=" + slaveId,
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
    AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

    const Try<JSON::Value> value = JSON::parse<JSON::Value>(response->body);

    ASSERT_SOME(value);

    Try<JSON::Object> object = value->as<JSON::Object>();

    Result<JSON::Array> array = object->find<JSON::Array>("slaves");
    ASSERT_SOME(array);
    EXPECT_EQ(1u, array->values.size());

    Try<JSON::Value> expected = JSON::parse(
        "{"
          "\"slaves\":"
            "[{"
                "\"id\":\"" + slaveId + "\""
            "}]"
        "}");

    ASSERT_SOME(expected);

    EXPECT_TRUE(value->contains(expected.get()));
  }

  // Stop agents while the master is down.
  master->reset();
  slave1.get()->terminate();
  slave1->reset();
  slave2.get()->terminate();
  slave2->reset();

  // Restart the master, now two agents should be in the 'recovered' state.
  master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Check if the second agent is in the 'recovered_slaves' field.
  {
    string slaveId = slave2RegisteredMessage->slave_id().value();

    Future<Response> response = process::http::get(
        master.get()->pid,
        "slaves?slave_id=" + slaveId,
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
    AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

    const Try<JSON::Value> value = JSON::parse<JSON::Value>(response->body);

    ASSERT_SOME(value);
    Try<JSON::Object> object = value->as<JSON::Object>();

    Result<JSON::Array> array = object->find<JSON::Array>("recovered_slaves");
    ASSERT_SOME(array);
    EXPECT_EQ(1u, array->values.size());

    Try<JSON::Value> expected = JSON::parse(
        "{"
          "\"recovered_slaves\":"
            "[{"
                "\"id\":\"" + slaveId + "\""
            "}]"
        "}");

    ASSERT_SOME(expected);

    EXPECT_TRUE(value->contains(expected.get()));
  }
}


// This test ensures that when a slave is recovered from the registry
// but does not re-register with the master, it is marked unreachable
// in the registry, the framework is informed that the slave is lost,
// and the slave is allowed to re-register.
TEST_F(MasterTest, RecoveredSlaveCanReregister)
{
  // Step 1: Start a master.
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.registry = "replicated_log";

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Step 2: Start a slave.
  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), master.get()->pid, _);

  // Reuse slaveFlags so both StartSlave() use the same work_dir.
  slave::Flags slaveFlags = this->CreateSlaveFlags();

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);

  // Step 3: Stop the slave while the master is down.
  master->reset();
  slave.get()->terminate();
  slave->reset();

  // Step 4: Restart the master.
  master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Step 5: Start a scheduler.
  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<Nothing> registered;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureSatisfy(&registered));

  driver.start();

  AWAIT_READY(registered);

  // Step 6: Advance the clock until the re-registration timeout
  // elapses, and expect the slave to be lost!
  Future<Nothing> slaveLost;
  EXPECT_CALL(sched, slaveLost(&driver, _))
    .WillOnce(FutureSatisfy(&slaveLost));

  Clock::pause();
  Clock::advance(masterFlags.agent_reregister_timeout);

  AWAIT_READY(slaveLost);

  JSON::Object stats = Metrics();
  EXPECT_EQ(1, stats.values["master/recovery_slave_removals"]);
  EXPECT_EQ(1, stats.values["master/slave_removals"]);
  EXPECT_EQ(1, stats.values["master/slave_removals/reason_unhealthy"]);
  EXPECT_EQ(0, stats.values["master/slave_removals/reason_unregistered"]);
  EXPECT_EQ(1, stats.values["master/slave_unreachable_completed"]);
  EXPECT_EQ(1, stats.values["master/slave_unreachable_scheduled"]);

  Clock::resume();

  // Step 7: Ensure the slave can re-register.
  Future<SlaveReregisteredMessage> slaveReregisteredMessage =
    FUTURE_PROTOBUF(SlaveReregisteredMessage(), master.get()->pid, _);

  // Expect a resource offer from the re-registered slave.
  Future<Nothing> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureSatisfy(&offers));

  detector = master.get()->createDetector();
  slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveReregisteredMessage);
  AWAIT_READY(offers);

  driver.stop();
  driver.join();
}


// This test ensures that when a master fails over and an agent does
// not reregister within the `agent_reregister_timeout`, the agent is
// marked unreachable; the framework should NOT receive a status
// update for any tasks running on the agent, but reconciliation
// should indicate the agent is unreachable.
TEST_F(MasterTest, UnreachableTaskAfterFailover)
{
  // Step 1: Start a master.
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.registry = "replicated_log";

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Step 2: Start a slave.
  StandaloneMasterDetector slaveDetector(master.get()->pid);
  slave::Flags agentFlags = CreateSlaveFlags();
  Try<Owned<cluster::Slave>> slave = StartSlave(&slaveDetector, agentFlags);
  ASSERT_SOME(slave);

  // Step 3: Start a scheduler.
  StandaloneMasterDetector schedDetector(master.get()->pid);
  MockScheduler sched;
  TestingMesosSchedulerDriver driver(&sched, &schedDetector);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  TaskInfo task = createTask(offers.get()[0], "sleep 100");

  Future<TaskStatus> startingStatus;
  Future<TaskStatus> runningStatus;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&startingStatus))
    .WillOnce(FutureArg<1>(&runningStatus))
    .WillRepeatedly(Return());

  Future<Nothing> statusUpdateAck1 = FUTURE_DISPATCH(
      slave.get()->pid, &Slave::_statusUpdateAcknowledgement);

  Future<Nothing> statusUpdateAck2 = FUTURE_DISPATCH(
      slave.get()->pid, &Slave::_statusUpdateAcknowledgement);

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(startingStatus);
  EXPECT_EQ(TASK_STARTING, startingStatus->state());
  EXPECT_EQ(task.task_id(), startingStatus->task_id());

  const SlaveID slaveId = startingStatus->slave_id();

  AWAIT_READY(statusUpdateAck1);

  AWAIT_READY(runningStatus);
  EXPECT_EQ(TASK_RUNNING, runningStatus->state());
  EXPECT_EQ(task.task_id(), runningStatus->task_id());

  AWAIT_READY(statusUpdateAck2);

  // Step 4: Simulate master failover. We leave the slave without a
  // master so it does not attempt to re-register.
  slaveDetector.appoint(None());

  master->reset();
  master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Cause the scheduler to re-register with the master.
  Future<Nothing> disconnected;
  EXPECT_CALL(sched, disconnected(&driver))
    .WillOnce(FutureSatisfy(&disconnected));

  Future<Nothing> registered;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureSatisfy(&registered));

  schedDetector.appoint(master.get()->pid);

  AWAIT_READY(disconnected);
  AWAIT_READY(registered);

  Future<Nothing> slaveLost;
  EXPECT_CALL(sched, slaveLost(&driver, _))
    .WillOnce(FutureSatisfy(&slaveLost));

  // Trigger the slave re-registration timeout.
  Clock::pause();
  Clock::advance(masterFlags.agent_reregister_timeout);
  TimeInfo unreachableTime = protobuf::getCurrentTime();

  // We expect to get a `slaveLost` signal; we do NOT expect to get a
  // status update for the task that was running on the slave.
  AWAIT_READY(slaveLost);

  // Reconciliation should return TASK_LOST, with `unreachable_time`
  // equal to the time when the re-registration timeout fired.
  TaskStatus status;
  status.mutable_task_id()->CopyFrom(task.task_id());
  status.mutable_slave_id()->CopyFrom(slaveId);
  status.set_state(TASK_STAGING); // Dummy value.

  Future<TaskStatus> reconcileUpdate1;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&reconcileUpdate1));

  driver.reconcileTasks({status});

  AWAIT_READY(reconcileUpdate1);
  EXPECT_EQ(TASK_LOST, reconcileUpdate1->state());
  EXPECT_EQ(TaskStatus::REASON_RECONCILIATION, reconcileUpdate1->reason());
  EXPECT_EQ(unreachableTime, reconcileUpdate1->unreachable_time());

  // Cause the slave to re-register with the master.
  Future<SlaveReregisteredMessage> slaveReregisteredMessage =
    FUTURE_PROTOBUF(SlaveReregisteredMessage(), _, _);

  slaveDetector.appoint(master.get()->pid);

  Clock::advance(agentFlags.registration_backoff_factor);
  AWAIT_READY(slaveReregisteredMessage);

  // The task should have returned to TASK_RUNNING. This is true even
  // for non-partition-aware frameworks, since we emulate the old
  // "non-strict registry" semantics.
  Future<TaskStatus> reconcileUpdate2;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&reconcileUpdate2));

  driver.reconcileTasks({status});

  AWAIT_READY(reconcileUpdate2);
  EXPECT_EQ(TASK_RUNNING, reconcileUpdate2->state());
  EXPECT_EQ(TaskStatus::REASON_RECONCILIATION, reconcileUpdate2->reason());

  Clock::resume();

  JSON::Object stats = Metrics();
  EXPECT_EQ(0, stats.values["master/tasks_lost"]);
  EXPECT_EQ(0, stats.values["master/tasks_unreachable"]);
  EXPECT_EQ(1, stats.values["master/tasks_running"]);
  EXPECT_EQ(1, stats.values["master/slave_unreachable_scheduled"]);
  EXPECT_EQ(1, stats.values["master/slave_unreachable_completed"]);
  EXPECT_EQ(1, stats.values["master/slave_removals"]);
  EXPECT_EQ(1, stats.values["master/slave_removals/reason_unhealthy"]);
  EXPECT_EQ(0, stats.values["master/slave_removals/reason_unregistered"]);
  EXPECT_EQ(1, stats.values["master/recovery_slave_removals"]);

  driver.stop();
  driver.join();
}


// This test ensures that slave removals during master recovery
// are rate limited.
TEST_F(MasterTest, RateLimitRecoveredSlaveRemoval)
{
  // Start a master.
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.registry = "replicated_log";

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), master.get()->pid, _);

  Owned<MasterDetector> detector = master.get()->createDetector();

  // Start a slave.
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);

  // Stop the slave while the master is down.
  master->reset();
  slave.get()->terminate();
  slave->reset();

  auto slaveRemovalLimiter = std::make_shared<MockRateLimiter>();

  // Return a pending future from the rate limiter.
  Future<Nothing> acquire;
  Promise<Nothing> promise;
  EXPECT_CALL(*slaveRemovalLimiter, acquire())
    .WillOnce(DoAll(FutureSatisfy(&acquire),
                    Return(promise.future())));

  // Restart the master.
  master = StartMaster(slaveRemovalLimiter, masterFlags);
  ASSERT_SOME(master);

  // Start a scheduler to ensure the master would notify
  // a framework about slave removal.
  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<Nothing> registered;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureSatisfy(&registered));

  Future<Nothing> slaveLost;
  EXPECT_CALL(sched, slaveLost(&driver, _))
    .WillOnce(FutureSatisfy(&slaveLost));

  driver.start();

  AWAIT_READY(registered);

  // Trigger the slave re-registration timeout.
  Clock::pause();
  Clock::advance(masterFlags.agent_reregister_timeout);

  // The master should attempt to acquire a permit.
  AWAIT_READY(acquire);

  // The removal should not occur before the permit is satisfied.
  Clock::settle();
  ASSERT_TRUE(slaveLost.isPending());

  // Once the permit is satisfied, the slave should be removed.
  promise.set(Nothing());
  AWAIT_READY(slaveLost);

  driver.stop();
  driver.join();
}


// This test ensures that slave removals that get scheduled during
// master recovery can be canceled if the slave re-registers.
TEST_F(MasterTest, CancelRecoveredSlaveRemoval)
{
  // Start a master.
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.registry = "replicated_log";

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), master.get()->pid, _);

  // Reuse slaveFlags so both StartSlave() use the same work_dir.
  slave::Flags slaveFlags = CreateSlaveFlags();

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);

  // Stop the slave while the master is down.
  master->reset();
  slave.get()->terminate();
  slave->reset();

  auto slaveRemovalLimiter = std::make_shared<MockRateLimiter>();

  // Return a pending future from the rate limiter.
  Future<Nothing> acquire;
  Promise<Nothing> promise;
  EXPECT_CALL(*slaveRemovalLimiter, acquire())
    .WillOnce(DoAll(FutureSatisfy(&acquire),
                    Return(promise.future())));

  // Restart the master.
  master = StartMaster(slaveRemovalLimiter, masterFlags);
  ASSERT_SOME(master);

  // Start a scheduler to ensure the master would notify
  // a framework about slave removal.
  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<Nothing> registered;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureSatisfy(&registered));

  Future<Nothing> slaveLost;
  EXPECT_CALL(sched, slaveLost(&driver, _))
    .WillRepeatedly(FutureSatisfy(&slaveLost));

  driver.start();

  AWAIT_READY(registered);

  // Trigger the slave re-registration timeout.
  Clock::pause();
  Clock::advance(masterFlags.agent_reregister_timeout);

  // The master should attempt to acquire a permit.
  AWAIT_READY(acquire);

  // The removal should not occur before the permit is satisfied.
  Clock::settle();
  ASSERT_TRUE(slaveLost.isPending());

  // Ignore resource offers from the re-registered slave.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(Return());

  Future<SlaveReregisteredMessage> slaveReregisteredMessage =
    FUTURE_PROTOBUF(SlaveReregisteredMessage(), master.get()->pid, _);

  // Restart the slave.
  detector = master.get()->createDetector();
  slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  Clock::advance(slaveFlags.registration_backoff_factor);
  AWAIT_READY(slaveReregisteredMessage);

  // Satisfy the rate limit permit. Ensure a removal does not occur!
  promise.set(Nothing());
  Clock::settle();
  ASSERT_TRUE(slaveLost.isPending());

  driver.stop();
  driver.join();
}


// This test ensures that when a slave is recovered from the registry
// and re-registers with the master, it is *not* removed after the
// re-registration timeout elapses.
TEST_F(MasterTest, RecoveredSlaveReregisters)
{
  // Step 1: Start a master.
  master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Step 2: Start a slave.
  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), master.get()->pid, _);

  // Reuse slaveFlags so both StartSlave() use the same work_dir.
  slave::Flags slaveFlags = this->CreateSlaveFlags();

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);

  // Step 3: Stop the slave while the master is down.
  master->reset();
  slave.get()->terminate();
  slave->reset();

  // Step 4: Restart the master.
  master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Step 5: Start a scheduler to ensure the master would notify
  // a framework, were a slave to be lost.
  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<Nothing> registered;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureSatisfy(&registered));

  // Ignore all offer related calls. The scheduler might receive
  // offerRescinded calls because the slave might re-register due to
  // ping timeout.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(Return());

  EXPECT_CALL(sched, offerRescinded(&driver, _))
    .WillRepeatedly(Return());

  driver.start();

  AWAIT_READY(registered);

  Future<SlaveReregisteredMessage> slaveReregisteredMessage =
    FUTURE_PROTOBUF(SlaveReregisteredMessage(), master.get()->pid, _);

  detector = master.get()->createDetector();
  slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveReregisteredMessage);

  // Step 6: Advance the clock and make sure the slave is not
  // removed!
  Future<Nothing> slaveLost;
  EXPECT_CALL(sched, slaveLost(&driver, _))
    .WillRepeatedly(FutureSatisfy(&slaveLost));

  Clock::pause();
  Clock::advance(masterFlags.agent_reregister_timeout);
  Clock::settle();

  ASSERT_TRUE(slaveLost.isPending());

  driver.stop();
  driver.join();
}


// This test checks that the master behaves correctly when a slave is
// in the process of reregistering after master failover when the
// agent failover timeout expires.
TEST_F(MasterTest, RecoveredSlaveReregisterThenUnreachableRace)
{
  master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), master.get()->pid, _);

  // Reuse slaveFlags so both StartSlave() use the same work_dir.
  slave::Flags slaveFlags = this->CreateSlaveFlags();

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);

  // Stop the slave while the master is down.
  master->reset();
  slave.get()->terminate();
  slave->reset();

  // Restart the master.
  master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Start the slave, which will cause it to reregister. Intercept the
  // next registry operation, which we expect to be slave reregistration.
  Future<ReregisterSlaveMessage> reregisterSlaveMessage =
    FUTURE_PROTOBUF(ReregisterSlaveMessage(), _, master.get()->pid);

  Future<Owned<master::Operation>> reregister;
  Promise<bool> reregisterContinue;
  EXPECT_CALL(*master.get()->registrar, apply(_))
    .WillOnce(DoAll(FutureArg<0>(&reregister),
                    Return(reregisterContinue.future())));

  detector = master.get()->createDetector();
  slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(reregisterSlaveMessage);

  AWAIT_READY(reregister);
  EXPECT_NE(
      nullptr,
      dynamic_cast<master::MarkSlaveReachable*>(
          reregister->get()));

  // Advance the clock to cause the agent reregister timeout to
  // expire. Because slave reregistration has already started, we do
  // NOT expect the master to mark the slave unreachable. Hence we
  // don't expect to see any registry operations.
  EXPECT_CALL(*master.get()->registrar, apply(_))
    .Times(0);

  Clock::pause();
  Clock::advance(masterFlags.agent_reregister_timeout);
  Clock::settle();
}


#ifdef MESOS_HAS_JAVA

class MasterZooKeeperTest : public MesosZooKeeperTest {};

// This test verifies that when the ZooKeeper cluster is lost,
// master, slave & scheduler all get informed.
TEST_F(MasterZooKeeperTest, LostZooKeeperCluster)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Future<process::Message> slaveRegisteredMessage =
    FUTURE_MESSAGE(Eq(SlaveRegisteredMessage().GetTypeName()), _, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, stringify(url.get()), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(Return()); // Ignore offers.

  Future<process::Message> frameworkRegisteredMessage =
    FUTURE_MESSAGE(Eq(FrameworkRegisteredMessage().GetTypeName()), _, _);

  driver.start();

  // Wait for the "registered" messages so that we know the master is
  // detected by everyone.
  AWAIT_READY(frameworkRegisteredMessage);
  AWAIT_READY(slaveRegisteredMessage);

  Future<Nothing> schedulerDisconnected;
  EXPECT_CALL(sched, disconnected(&driver))
    .WillOnce(FutureSatisfy(&schedulerDisconnected));

  // Need to drop these two dispatches because otherwise the master
  // will EXIT.
  Future<Nothing> masterDetected = DROP_DISPATCH(_, &Master::detected);
  Future<Nothing> lostCandidacy = DROP_DISPATCH(_, &Master::lostCandidacy);

  Future<Nothing> slaveDetected = FUTURE_DISPATCH(_, &Slave::detected);

  server->shutdownNetwork();

  Clock::pause();

  while (schedulerDisconnected.isPending() ||
         masterDetected.isPending() ||
         slaveDetected.isPending() ||
         lostCandidacy.isPending()) {
    Clock::advance(MASTER_CONTENDER_ZK_SESSION_TIMEOUT);
    Clock::settle();
  }

  Clock::resume();

  // Master, slave and scheduler all lose the leading master.
  AWAIT_READY(schedulerDisconnected);
  AWAIT_READY(masterDetected);
  AWAIT_READY(lostCandidacy);
  AWAIT_READY(slaveDetected);

  driver.stop();
  driver.join();
}


// This test verifies that the Address inside MasterInfo
// is populated correctly, during master initialization.
TEST_F(MasterZooKeeperTest, MasterInfoAddress)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<MasterInfo> masterInfo;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<2>(&masterInfo));

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(Return()); // Ignore offers.

  driver.start();
  AWAIT_READY(masterInfo);

  const Address& address = masterInfo->address();
  EXPECT_EQ(stringify(master.get()->pid.address.ip), address.ip());
  EXPECT_EQ(master.get()->pid.address.port, address.port());

  // Protect from failures on those hosts where
  // hostname cannot be resolved.
  if (master.get()->pid.address.hostname().isSome()) {
    ASSERT_EQ(master.get()->pid.address.hostname().get(), address.hostname());
  }

  driver.stop();
  driver.join();
}

#endif // MESOS_HAS_JAVA


// This test ensures that when a master fails over, tasks that belong
// to frameworks that have not re-registered will be reported in the
// "/state" endpoint. The framework itself should have the "recovered"
// field set to true.
TEST_F(MasterTest, RecoveredFramework)
{
  // Start a master.
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);
  StandaloneMasterDetector detector(master.get()->pid);

  // NOTE: After the master fails over, we need the agent to register
  // before the framework retries registration. Hence, the backoff
  // factor has to be smaller than the framework registration backoff
  // factor, but still > 0 so that the registration backoff code
  // paths are exercised.
  slave::Flags agentFlags = CreateSlaveFlags();
  agentFlags.registration_backoff_factor = Nanoseconds(10);

  // Start a slave.
  Try<Owned<cluster::Slave>> slave =
    StartSlave(&detector, &containerizer, agentFlags);
  ASSERT_SOME(slave);

  // Create a task on the slave.
  MockScheduler sched;
  TestingMesosSchedulerDriver driver(&sched, &detector);

  FrameworkID frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(SaveArg<1>(&frameworkId))
    .WillRepeatedly(Return()); // Ignore subsequent events.

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(LaunchTasks(DEFAULT_EXECUTOR_INFO, 1, 1, 64, "*"))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  EXPECT_CALL(exec, registered(_, _, _, _));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.start();

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status->state());

  // Get the master's state.
  Future<Response> response1 = process::http::get(
      master.get()->pid,
      "state",
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response1);
  AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response1);

  Try<JSON::Object> parse1 = JSON::parse<JSON::Object>(response1->body);
  ASSERT_SOME(parse1);

  JSON::Array frameworks1 = parse1->values["frameworks"].as<JSON::Array>();
  ASSERT_EQ(1u, frameworks1.values.size());

  JSON::Object activeFramework1 = frameworks1.values.front().as<JSON::Object>();

  EXPECT_EQ(
      frameworkId.value(),
      activeFramework1.values["id"].as<JSON::String>().value);

  EXPECT_TRUE(activeFramework1.values["active"].as<JSON::Boolean>().value);
  EXPECT_TRUE(activeFramework1.values["connected"].as<JSON::Boolean>().value);
  EXPECT_FALSE(activeFramework1.values["recovered"].as<JSON::Boolean>().value);

  JSON::Array activeTasks1 = activeFramework1.values["tasks"].as<JSON::Array>();
  EXPECT_EQ(1u, activeTasks1.values.size());

  JSON::Array unregisteredFrameworks1 =
    parse1->values["unregistered_frameworks"].as<JSON::Array>();

  EXPECT_TRUE(unregisteredFrameworks1.values.empty());

  EXPECT_TRUE(parse1->values["orphan_tasks"].as<JSON::Array>().values.empty());

  EXPECT_CALL(sched, disconnected(&driver));

  // Stop the master.
  PID<Master> originalPid = master.get()->pid;

  Future<SlaveReregisteredMessage> slaveReregisteredMessage =
    FUTURE_PROTOBUF(SlaveReregisteredMessage(), originalPid, _);

  // Drop the subscribe call to delay the framework from
  // re-registration.
  // Grab the stuff we need to replay the subscribe call.
  Future<mesos::scheduler::Call> subscribeCall = DROP_CALL(
      mesos::scheduler::Call(),
      mesos::scheduler::Call::SUBSCRIBE,
      _,
      _);

  Clock::pause();

  // The master failover.
  master->reset();
  master = StartMaster();
  ASSERT_SOME(master);

  // Settle the clock to ensure the master finishes
  // executing _recover().
  Clock::settle();

  // Simulate a new master detected event to the slave and the framework.
  detector.appoint(master.get()->pid);

  Clock::advance(agentFlags.registration_backoff_factor);
  AWAIT_READY(slaveReregisteredMessage);
  AWAIT_READY(subscribeCall);

  // Get the master's state.
  Future<Response> response2 = process::http::get(
      master.get()->pid,
      "state",
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response2);
  AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response2);

  Try<JSON::Object> parse2 = JSON::parse<JSON::Object>(response2->body);
  ASSERT_SOME(parse2);

  // Check that there is a single recovered framework, a single active
  // task, and no orphan tasks.

  JSON::Array frameworks2 = parse2->values["frameworks"].as<JSON::Array>();
  ASSERT_EQ(1u, frameworks2.values.size());

  JSON::Object activeFramework2 = frameworks2.values.front().as<JSON::Object>();

  EXPECT_EQ(
      frameworkId.value(),
      activeFramework2.values["id"].as<JSON::String>().value);

  EXPECT_FALSE(activeFramework2.values["active"].as<JSON::Boolean>().value);
  EXPECT_FALSE(activeFramework2.values["connected"].as<JSON::Boolean>().value);
  EXPECT_TRUE(activeFramework2.values["recovered"].as<JSON::Boolean>().value);

  JSON::Array activeTasks2 = activeFramework2.values["tasks"].as<JSON::Array>();
  EXPECT_EQ(activeTasks1, activeTasks2);

  JSON::Array unregisteredFrameworks2 =
    parse2->values["unregistered_frameworks"].as<JSON::Array>();

  EXPECT_TRUE(unregisteredFrameworks2.values.empty());

  EXPECT_TRUE(parse2->values["orphan_tasks"].as<JSON::Array>().values.empty());

  Future<FrameworkRegisteredMessage> frameworkRegisteredMessage =
    FUTURE_PROTOBUF(FrameworkRegisteredMessage(), _, _);

  // Advance the clock to let the framework re-register with the master.
  Clock::advance(Seconds(1));
  Clock::settle();
  Clock::resume();

  AWAIT_READY(frameworkRegisteredMessage);

  // Get the master's state.
  Future<Response> response3 = process::http::get(
      master.get()->pid,
      "state",
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response3);
  AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response3);

  Try<JSON::Object> parse3 = JSON::parse<JSON::Object>(response3->body);
  ASSERT_SOME(parse3);

  // The framework should no longer be listed as recovered.

  JSON::Array frameworks3 = parse3->values["frameworks"].as<JSON::Array>();
  ASSERT_EQ(1u, frameworks3.values.size());

  JSON::Object activeFramework3 = frameworks3.values.front().as<JSON::Object>();

  EXPECT_EQ(
      frameworkId.value(),
      activeFramework3.values["id"].as<JSON::String>().value);

  EXPECT_TRUE(activeFramework3.values["active"].as<JSON::Boolean>().value);
  EXPECT_TRUE(activeFramework3.values["connected"].as<JSON::Boolean>().value);
  EXPECT_FALSE(activeFramework3.values["recovered"].as<JSON::Boolean>().value);

  JSON::Array activeTasks3 = activeFramework3.values["tasks"].as<JSON::Array>();
  EXPECT_EQ(activeTasks1, activeTasks3);

  JSON::Array unregisteredFrameworks3 =
    parse3->values["unregistered_frameworks"].as<JSON::Array>();

  EXPECT_TRUE(unregisteredFrameworks3.values.empty());

  EXPECT_TRUE(parse3->values["orphan_tasks"].as<JSON::Array>().values.empty());

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();
}


// This test verifies that a framework that has not yet re-registered
// after a master failover doesn't show up multiple times in
// "frameworks" when querying "/state" or "/frameworks" endpoints. This
// is to catch any regressions for MESOS-4973 and MESOS-6461.
TEST_F(MasterTest, OrphanTasksMultipleAgents)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  StandaloneMasterDetector slavesDetector(master.get()->pid);

  MockExecutor exec1(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer1(&exec1);

  // Start the first slave and launch a task.

  Try<Owned<cluster::Slave>> slave1 =
    StartSlave(&slavesDetector, &containerizer1);

  ASSERT_SOME(slave1);

  StandaloneMasterDetector schedDetector(master.get()->pid);

  MockScheduler sched;
  TestingMesosSchedulerDriver driver(&sched, &schedDetector);

  FrameworkID frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(SaveArg<1>(&frameworkId))
    .WillRepeatedly(Return()); // Ignore subsequent events.

  Future<vector<Offer>> offers1;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers1));

  driver.start();

  AWAIT_READY(offers1);
  ASSERT_FALSE(offers1->empty());

  TaskInfo task1 =
    createTask(offers1.get()[0], "sleep 100", DEFAULT_EXECUTOR_ID);

  EXPECT_CALL(exec1, registered(_, _, _, _));

  EXPECT_CALL(exec1, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> status1;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status1));

  driver.launchTasks(offers1.get()[0].id(), {task1});

  AWAIT_READY(status1);
  EXPECT_EQ(TASK_RUNNING, status1->state());

  // Start the second slave and launch a task.

  Future<vector<Offer>> offers2;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers2))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  MockExecutor exec2(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer2(&exec2);

  Try<Owned<cluster::Slave>> slave2 = StartSlave(
    &slavesDetector, &containerizer2);

  ASSERT_SOME(slave2);

  AWAIT_READY(offers2);
  ASSERT_FALSE(offers2->empty());

  TaskInfo task2 =
    createTask(offers2.get()[0], "sleep 100", DEFAULT_EXECUTOR_ID);

  EXPECT_CALL(exec2, registered(_, _, _, _));

  EXPECT_CALL(exec2, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> status2;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status2))
    .WillRepeatedly(Return()); // Ignore subsequent updates.

  driver.launchTasks(offers2.get()[0].id(), {task2});

  AWAIT_READY(status2);
  EXPECT_EQ(TASK_RUNNING, status2->state());

  Future<SlaveReregisteredMessage> slaveReregisteredMessage1 =
    FUTURE_PROTOBUF(SlaveReregisteredMessage(), _, slave1.get()->pid);

  Future<SlaveReregisteredMessage> slaveReregisteredMessage2 =
    FUTURE_PROTOBUF(SlaveReregisteredMessage(), _, slave2.get()->pid);

  // Failover the master.
  master->reset();
  master = StartMaster();
  ASSERT_SOME(master);

  // Simulate a new master detected event to the slaves (but not the scheduler).
  slavesDetector.appoint(master.get()->pid);

  AWAIT_READY(slaveReregisteredMessage1);
  AWAIT_READY(slaveReregisteredMessage2);

  // Ensure that there are 2 tasks and 1 recovered framework in
  // "/state" endpoint.
  {
    Future<Response> response = process::http::get(
        master.get()->pid,
        "state",
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
    AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
    ASSERT_SOME(parse);

    JSON::Array frameworks =
      parse->values["frameworks"].as<JSON::Array>();
    JSON::Array orphanTasks =
      parse->values["orphan_tasks"].as<JSON::Array>();
    JSON::Array unregisteredFrameworks =
      parse->values["unregistered_frameworks"].as<JSON::Array>();

    ASSERT_EQ(1u, frameworks.values.size());
    EXPECT_TRUE(orphanTasks.values.empty());
    EXPECT_TRUE(unregisteredFrameworks.values.empty());

    JSON::Object framework = frameworks.values.front().as<JSON::Object>();

    EXPECT_TRUE(framework.values["recovered"].as<JSON::Boolean>().value);
  }

  // Ensure that there is 1 recovered framework in "/frameworks" endpoint.
  {
    Future<Response> response = process::http::get(
        master.get()->pid,
        "frameworks",
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
    AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
    ASSERT_SOME(parse);

    JSON::Array frameworks =
      parse->values["frameworks"].as<JSON::Array>();
    JSON::Array unregisteredFrameworks =
      parse->values["unregistered_frameworks"].as<JSON::Array>();

    ASSERT_EQ(1u, frameworks.values.size());
    EXPECT_TRUE(unregisteredFrameworks.values.empty());

    JSON::Object framework = frameworks.values.front().as<JSON::Object>();

    EXPECT_TRUE(framework.values["recovered"].as<JSON::Boolean>().value);
  }

  EXPECT_CALL(exec1, shutdown(_))
    .Times(AtMost(1));

  EXPECT_CALL(exec2, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();
}


// This test verifies that when a framework tears down with no tasks
// still alive or pending acknowledgement, it doesn't show up in the
// /state endpoint's "unregistered_frameworks" list. This is to catch
// any regression to MESOS-4975.
TEST_F(MasterTest, UnregisteredFrameworksAfterTearDown)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Wait until the master fully processes slave registration before
  // connecting the framework. This is to reproduce the condition in
  // MESOS-4975.
  Future<Message> slaveRegisteredMessage = FUTURE_MESSAGE(
      Eq(SlaveRegisteredMessage().GetTypeName()), _, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);

  // Give `frameworkInfo` a framework ID to simulate a failed-over
  // framework (with no unacknowledged tasks). This is to reproduce
  // the condition in MESOS-4975.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.mutable_id()->set_value("framework1");

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<Nothing> registered;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureSatisfy(&registered));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  // Wait until the master registers the framework and sends an offer,
  // before we shutdown the framework.
  AWAIT_READY(registered);
  AWAIT_READY(offers);

  driver.stop();
  driver.join();

  // Ensure that there are no unregistered frameworks in "/state" endpoint.
  Future<Response> response = process::http::get(
      master.get()->pid,
      "state",
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

  Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
  ASSERT_SOME(parse);

  JSON::Object state = parse.get();

  JSON::Array unregisteredFrameworks =
    state.values["unregistered_frameworks"].as<JSON::Array>();

  EXPECT_TRUE(unregisteredFrameworks.values.empty());
}


// This tests /tasks endpoint to return correct task information.
TEST_F(MasterTest, TasksEndpoint)
{
  master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), &containerizer);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  process::Queue<Offer> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(EnqueueOffers(&offers));

  driver.start();

  Future<Offer> offer = offers.get();
  AWAIT_READY(offer);

  // Launch two tasks.
  TaskInfo task1;
  task1.set_name("test1");
  task1.mutable_task_id()->set_value("1");
  task1.mutable_slave_id()->MergeFrom(offer->slave_id());
  task1.mutable_resources()->MergeFrom(
      Resources::parse("cpus:0.1;mem:12").get());
  task1.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  TaskInfo task2;
  task2.set_name("test2");
  task2.mutable_task_id()->set_value("2");
  task2.mutable_slave_id()->MergeFrom(offer->slave_id());
  task2.mutable_resources()->MergeFrom(
      Resources::parse("cpus:0.1;mem:12").get());
  task2.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  vector<TaskInfo> tasks;
  tasks.push_back(task1);
  tasks.push_back(task2);

  EXPECT_CALL(exec, registered(_, _, _, _));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> status1, status2;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status1))
    .WillOnce(FutureArg<1>(&status2));

  driver.launchTasks(offer->id(), tasks);

  AWAIT_READY(status1);
  EXPECT_EQ(TASK_RUNNING, status1->state());
  EXPECT_TRUE(status1->has_executor_id());
  EXPECT_EQ(exec.id, status1->executor_id());

  AWAIT_READY(status2);
  EXPECT_EQ(TASK_RUNNING, status2->state());
  EXPECT_TRUE(status2->has_executor_id());
  EXPECT_EQ(exec.id, status2->executor_id());

  // Testing the '/master/tasks' endpoint without parameters,
  // which returns information about all tasks.
  {
    Future<Response> response = process::http::get(
        master.get()->pid,
        "tasks",
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
    AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

    Try<JSON::Value> value = JSON::parse<JSON::Value>(response->body);
    ASSERT_SOME(value);

    // Two possible orderings of the result.
    Try<JSON::Value> expected1 = JSON::parse(
        "{"
          "\"tasks\":"
            "[{"
                "\"executor_id\":\"default\","
                "\"framework_id\":\"" + frameworkId->value() + "\","
                "\"id\":\"1\","
                "\"name\":\"test1\","
                "\"state\":\"TASK_RUNNING\""
              "},{"
                "\"executor_id\":\"default\","
                "\"framework_id\":\"" + frameworkId->value() + "\","
                "\"id\":\"2\","
                "\"name\":\"test2\","
                "\"state\":\"TASK_RUNNING\""
            "}]"
        "}");

    Try<JSON::Value> expected2 = JSON::parse(
        "{"
          "\"tasks\":"
            "[{"
                "\"executor_id\":\"default\","
                "\"framework_id\":\"" + frameworkId->value() + "\","
                "\"id\":\"2\","
                "\"name\":\"test2\","
                "\"state\":\"TASK_RUNNING\""
              "},{"
                "\"executor_id\":\"default\","
                "\"framework_id\":\"" + frameworkId->value() + "\","
                "\"id\":\"1\","
                "\"name\":\"test1\","
                "\"state\":\"TASK_RUNNING\""
            "}]"
        "}");

    ASSERT_SOME(expected1);
    ASSERT_SOME(expected2);

    EXPECT_TRUE(
        value->contains(expected1.get()) ||
        value->contains(expected2.get()));
  }

  // Testing the query for a specific task.
  {
    Future<Response> response = process::http::get(
        master.get()->pid,
        "tasks?task_id=1;framework_id=" + frameworkId->value(),
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
    AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

    Try<JSON::Value> value = JSON::parse<JSON::Value>(response->body);
    ASSERT_SOME(value);

    JSON::Object object = value->as<JSON::Object>();
    Result<JSON::Array> taskArray = object.find<JSON::Array>("tasks");
    ASSERT_SOME(taskArray);

    EXPECT_TRUE(taskArray->values.size() == 1);

    Try<JSON::Value> expected = JSON::parse(
        "{"
          "\"tasks\":"
            "[{"
                "\"executor_id\":\"default\","
                "\"framework_id\":\"" + frameworkId->value() + "\","
                "\"id\":\"1\","
                "\"name\":\"test1\","
                "\"state\":\"TASK_RUNNING\""
            "}]"
        "}");

    ASSERT_SOME(expected);
    EXPECT_TRUE(value->contains(expected.get()));
  }

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();
}


// This test verifies that the master will strip ephemeral ports
// resource from offers so that frameworks cannot see it.
TEST_F(MasterTest, IgnoreEphemeralPortsResource)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  string resourcesWithoutEphemeralPorts =
    "cpus:2;gpus:0;mem:1024;disk:1024;ports:[31000-32000]";

  string resourcesWithEphemeralPorts =
    resourcesWithoutEphemeralPorts + ";ephemeral_ports:[30001-30999]";

  slave::Flags flags = CreateSlaveFlags();
  flags.resources = resourcesWithEphemeralPorts;

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  AWAIT_READY(offers);
  ASSERT_EQ(1u, offers->size());

  EXPECT_EQ(
      Resources(offers.get()[0].resources()),
      allocatedResources(
          Resources::parse(resourcesWithoutEphemeralPorts).get(),
          DEFAULT_FRAMEWORK_INFO.roles(0)));

  driver.stop();
  driver.join();
}


#ifdef ENABLE_PORT_MAPPING_ISOLATOR
TEST_F(MasterTest, MaxExecutorsPerSlave)
{
  master::Flags flags = CreateMasterFlags();
  flags.max_executors_per_agent = 0;

  Try<Owned<cluster::Master>> master = StartMaster(flags);
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), &containerizer);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<MasterInfo> masterInfo;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<2>(&masterInfo));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .Times(0);

  driver.start();

  AWAIT_READY(masterInfo);
  EXPECT_EQ(master.get()->pid.address.port, masterInfo->port());
  EXPECT_EQ(
      master.get()->pid.address.ip, net::IP(ntohl(masterInfo->ip())));

  driver.stop();
  driver.join();
}
#endif  // ENABLE_PORT_MAPPING_ISOLATOR


// This test verifies that when the Framework has not responded to
// an offer within the default timeout, the offer is rescinded.
TEST_F(MasterTest, OfferTimeout)
{
  master::Flags masterFlags = MesosTest::CreateMasterFlags();
  masterFlags.offer_timeout = Seconds(30);
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<Nothing> registered;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureSatisfy(&registered));

  Future<vector<Offer>> offers1;
  Future<vector<Offer>> offers2;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers1))
    .WillOnce(FutureArg<1>(&offers2));

  // Expect offer rescinded.
  Future<Nothing> offerRescinded;
  EXPECT_CALL(sched, offerRescinded(&driver, _))
    .WillOnce(FutureSatisfy(&offerRescinded));

  Future<Nothing> recoverResources =
    FUTURE_DISPATCH(_, &MesosAllocatorProcess::recoverResources);

  driver.start();

  AWAIT_READY(registered);
  AWAIT_READY(offers1);
  ASSERT_EQ(1u, offers1->size());

  // Now advance the clock, we need to resume it afterwards to
  // allow the allocator to make a new allocation decision.
  Clock::pause();
  Clock::advance(masterFlags.offer_timeout.get());
  Clock::resume();

  AWAIT_READY(offerRescinded);

  AWAIT_READY(recoverResources);

  // Advance the clock and trigger a batch allocation.
  Clock::pause();
  Clock::advance(masterFlags.allocation_interval);
  Clock::resume();

  // Expect that the resources are re-offered to the framework after
  // the rescind.
  AWAIT_READY(offers2);
  ASSERT_EQ(1u, offers2->size());

  EXPECT_EQ(offers1.get()[0].resources(), offers2.get()[0].resources());

  driver.stop();
  driver.join();
}


// Offer should not be rescinded if it's accepted.
TEST_F(MasterTest, OfferNotRescindedOnceUsed)
{
  master::Flags masterFlags = MesosTest::CreateMasterFlags();
  masterFlags.offer_timeout = Seconds(30);
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), &containerizer);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<Nothing> registered;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureSatisfy(&registered));

  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillOnce(LaunchTasks(DEFAULT_EXECUTOR_INFO, 1, 1, 64, "*"))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  EXPECT_CALL(exec, registered(_, _, _, _));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status))
    .WillRepeatedly(Return()); // Ignore subsequent updates.

  // We don't expect any rescinds if the offer has been accepted.
  EXPECT_CALL(sched, offerRescinded(&driver, _))
    .Times(0);

  driver.start();
  AWAIT_READY(registered);

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status->state());

  // Now advance to the offer timeout, we need to settle the clock to
  // ensure that the offer rescind timeout would be processed
  // if triggered.
  Clock::pause();
  Clock::advance(masterFlags.offer_timeout.get());
  Clock::settle();

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();
}


// Offer should not be rescinded if it has been declined.
TEST_F(MasterTest, OfferNotRescindedOnceDeclined)
{
  master::Flags masterFlags = MesosTest::CreateMasterFlags();
  masterFlags.offer_timeout = Seconds(30);
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), &containerizer);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<Nothing> registered;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureSatisfy(&registered));

  EXPECT_CALL(sched, resourceOffers(_, _))
    .WillRepeatedly(DeclineOffers()); // Decline all offers.

  Future<mesos::scheduler::Call> declineCall = FUTURE_CALL(
      mesos::scheduler::Call(), mesos::scheduler::Call::DECLINE, _, _);

  EXPECT_CALL(sched, offerRescinded(&driver, _))
    .Times(0);

  driver.start();
  AWAIT_READY(registered);

  // Wait for the framework to decline the offers.
  AWAIT_READY(declineCall);

  // Now advance to the offer timeout, we need to settle the clock to
  // ensure that the offer rescind timeout would be processed
  // if triggered.
  Clock::pause();
  Clock::advance(masterFlags.offer_timeout.get());
  Clock::settle();

  driver.stop();
  driver.join();
}


// This test ensures that the master releases resources for tasks
// when they terminate, even if no acknowledgements occur.
TEST_F(MasterTest, UnacknowledgedTerminalTask)
{
  master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "cpus:1;mem:64";

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &containerizer, slaveFlags);
  ASSERT_SOME(slave);

  // Launch a framework and get a task into a terminal state.
  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer>> offers1;
  Future<vector<Offer>> offers2;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(DoAll(FutureArg<1>(&offers1),
                    LaunchTasks(DEFAULT_EXECUTOR_INFO, 1, 1, 64, "*")))
    .WillOnce(FutureArg<1>(&offers2)); // Ignore subsequent offers.

  EXPECT_CALL(exec, registered(_, _, _, _));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_FINISHED));

  // Capture the status update message from the slave to the master.
  Future<StatusUpdateMessage> update =
    FUTURE_PROTOBUF(StatusUpdateMessage(), _, master.get()->pid);

  // Drop the status updates forwarded to the framework to ensure
  // that the task remains terminal and unacknowledged in the master.
  DROP_PROTOBUFS(StatusUpdateMessage(), master.get()->pid, _);

  driver.start();

  // Wait until the framework is registered.
  AWAIT_READY(frameworkId);
  AWAIT_READY(offers1);

  // Once the update is sent, the master should re-offer the
  // resources consumed by the task.
  AWAIT_READY(update);

  // Don't wait around for the allocation interval.
  Clock::pause();
  Clock::advance(masterFlags.allocation_interval);
  Clock::resume();

  AWAIT_READY(offers2);

  ASSERT_FALSE(offers1->empty());
  ASSERT_FALSE(offers2->empty());

  // Ensure we get all of the resources back.
  EXPECT_EQ(offers1.get()[0].resources(), offers2.get()[0].resources());

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();
}


// This test ensures that the master releases resources for a
// terminated task even when it receives a non-terminal update (with
// latest state set).
TEST_F(MasterTest, ReleaseResourcesForTerminalTaskWithPendingUpdates)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "cpus:1;mem:64";

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), &containerizer, slaveFlags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(LaunchTasks(DEFAULT_EXECUTOR_INFO, 1, 1, 64, "*"))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  ExecutorDriver* execDriver;
  EXPECT_CALL(exec, registered(_, _, _, _))
    .WillOnce(SaveArg<0>(&execDriver));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  // Drop all the updates from master to scheduler.
  DROP_PROTOBUFS(StatusUpdateMessage(), master.get()->pid, _);

  Future<StatusUpdateMessage> statusUpdateMessage =
    FUTURE_PROTOBUF(StatusUpdateMessage(), _, master.get()->pid);

  Future<Nothing> ___statusUpdate = FUTURE_DISPATCH(_, &Slave::___statusUpdate);

  driver.start();

  // Wait until TASK_RUNNING is sent to the master.
  AWAIT_READY(statusUpdateMessage);

  // Ensure task status update manager handles TASK_RUNNING update.
  AWAIT_READY(___statusUpdate);

  Future<Nothing> ___statusUpdate2 =
    FUTURE_DISPATCH(_, &Slave::___statusUpdate);

  // Now send TASK_FINISHED update.
  TaskStatus finishedStatus;
  finishedStatus = statusUpdateMessage->update().status();
  finishedStatus.set_state(TASK_FINISHED);
  execDriver->sendStatusUpdate(finishedStatus);

  // Ensure task status update manager handles TASK_FINISHED update.
  AWAIT_READY(___statusUpdate2);

  Future<Nothing> recoverResources = FUTURE_DISPATCH(
      _, &MesosAllocatorProcess::recoverResources);

  // Advance the clock so that the task status update manager resends
  // TASK_RUNNING update with 'latest_state' as TASK_FINISHED.
  Clock::pause();
  Clock::advance(slave::STATUS_UPDATE_RETRY_INTERVAL_MIN);
  Clock::resume();

  // Ensure the resources are recovered.
  AWAIT_READY(recoverResources);

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();
}


TEST_F(MasterTest, StateEndpoint)
{
  master::Flags flags = CreateMasterFlags();

  flags.hostname = "localhost";
  flags.cluster = "test-cluster";

  // Capture the start time deterministically.
  Clock::pause();

  Try<Owned<cluster::Master>> master = StartMaster(flags);
  ASSERT_SOME(master);

  Future<Response> response = process::http::get(
      master.get()->pid,
      "state",
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

  Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
  ASSERT_SOME(parse);

  JSON::Object state = parse.get();

  EXPECT_EQ(MESOS_VERSION, state.values["version"]);

  if (build::GIT_SHA.isSome()) {
    EXPECT_EQ(build::GIT_SHA.get(), state.values["git_sha"]);
  }

  if (build::GIT_BRANCH.isSome()) {
    EXPECT_EQ(build::GIT_BRANCH.get(), state.values["git_branch"]);
  }

  if (build::GIT_TAG.isSome()) {
    EXPECT_EQ(build::GIT_TAG.get(), state.values["git_tag"]);
  }

  EXPECT_EQ(build::DATE, state.values["build_date"]);
  EXPECT_EQ(build::TIME, state.values["build_time"]);
  EXPECT_EQ(build::USER, state.values["build_user"]);

  ASSERT_TRUE(state.values["start_time"].is<JSON::Number>());
  EXPECT_EQ(
      static_cast<int>(Clock::now().secs()),
      state.values["start_time"].as<JSON::Number>().as<int>());

  ASSERT_TRUE(state.values["id"].is<JSON::String>());
  EXPECT_NE("", state.values["id"].as<JSON::String>().value);

  EXPECT_EQ(stringify(master.get()->pid), state.values["pid"]);
  EXPECT_EQ(flags.hostname.get(), state.values["hostname"]);

  JSON::Object leader = state.values["leader_info"].as<JSON::Object>();

  EXPECT_EQ(flags.hostname.get(), leader.values["hostname"]);
  EXPECT_EQ(
      master.get()->pid.address.port,
      leader.values["port"].as<JSON::Number>().as<int>());

  EXPECT_EQ(0, state.values["activated_slaves"]);
  EXPECT_EQ(0, state.values["deactivated_slaves"]);

  EXPECT_EQ(flags.cluster.get(), state.values["cluster"]);

  // TODO(bmahler): Test "log_dir", "external_log_file".

  // TODO(bmahler): Ensure this contains all the flags.
  ASSERT_TRUE(state.values["flags"].is<JSON::Object>());
  EXPECT_FALSE(state.values["flags"].as<JSON::Object>().values.empty());

  ASSERT_TRUE(state.values["slaves"].is<JSON::Array>());
  EXPECT_TRUE(state.values["slaves"].as<JSON::Array>().values.empty());

  ASSERT_TRUE(state.values["orphan_tasks"].is<JSON::Array>());
  EXPECT_TRUE(state.values["orphan_tasks"].as<JSON::Array>().values.empty());

  ASSERT_TRUE(state.values["frameworks"].is<JSON::Array>());
  EXPECT_TRUE(state.values["frameworks"].as<JSON::Array>().values.empty());

  ASSERT_TRUE(
      state.values["completed_frameworks"].is<JSON::Array>());
  EXPECT_TRUE(
      state.values["completed_frameworks"].as<JSON::Array>().values.empty());

  ASSERT_TRUE(
      state.values["unregistered_frameworks"].is<JSON::Array>());
  EXPECT_TRUE(
      state.values["unregistered_frameworks"].as<JSON::Array>().values.empty());

  ASSERT_TRUE(state.values["capabilities"].is<JSON::Array>());
  EXPECT_TRUE(state.values["capabilities"].as<JSON::Array>().values.empty());
}


// This test ensures that the framework's information is included in
// the master's state endpoint.
//
// TODO(bmahler): This only looks at capabilities and the webui URL
// currently; add more to this test.
TEST_F(MasterTest, StateEndpointFrameworkInfo)
{
  Clock::pause();

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  slave::Flags agentFlags = CreateSlaveFlags();
  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), agentFlags);
  ASSERT_SOME(slave);

  Clock::advance(agentFlags.registration_backoff_factor);
  Clock::advance(agentFlags.authentication_backoff_factor);
  AWAIT_READY(slaveRegisteredMessage);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.clear_capabilities();

  frameworkInfo.set_webui_url("http://localhost:8080/");

  vector<FrameworkInfo::Capability::Type> capabilities = {
    FrameworkInfo::Capability::REVOCABLE_RESOURCES,
    FrameworkInfo::Capability::TASK_KILLING_STATE,
    FrameworkInfo::Capability::GPU_RESOURCES,
    FrameworkInfo::Capability::PARTITION_AWARE,
    FrameworkInfo::Capability::MULTI_ROLE,
    FrameworkInfo::Capability::RESERVATION_REFINEMENT,
  };

  foreach (FrameworkInfo::Capability::Type capability, capabilities) {
    frameworkInfo.add_capabilities()->set_type(capability);
  }

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<Nothing> registered;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureSatisfy(&registered));

  Future<Nothing> resourceOffers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureSatisfy(&resourceOffers));

  driver.start();

  AWAIT_READY(registered);
  AWAIT_READY(resourceOffers);

  Future<Response> response = process::http::get(
      master.get()->pid,
      "state",
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  Try<JSON::Object> object = JSON::parse<JSON::Object>(response->body);
  ASSERT_SOME(object);

  ASSERT_EQ(1u, object->values.count("frameworks"));
  JSON::Array frameworks = object->values["frameworks"].as<JSON::Array>();

  ASSERT_EQ(1u, frameworks.values.size());
  ASSERT_TRUE(frameworks.values.front().is<JSON::Object>());

  JSON::Object framework = frameworks.values.front().as<JSON::Object>();

  ASSERT_EQ(1u, framework.values.count("webui_url"));
  ASSERT_TRUE(framework.values["webui_url"].is<JSON::String>());
  EXPECT_EQ("http://localhost:8080/",
            framework.values["webui_url"].as<JSON::String>().value);

  ASSERT_EQ(1u, framework.values.count("capabilities"));
  ASSERT_TRUE(framework.values["capabilities"].is<JSON::Array>());

  vector<FrameworkInfo::Capability::Type> actual;

  foreach (const JSON::Value& capability,
           framework.values["capabilities"].as<JSON::Array>().values) {
    FrameworkInfo::Capability::Type type;

    ASSERT_TRUE(capability.is<JSON::String>());
    ASSERT_TRUE(
        FrameworkInfo::Capability::Type_Parse(
            capability.as<JSON::String>().value,
            &type));

    actual.push_back(type);
  }

  EXPECT_EQ(capabilities, actual);

  ASSERT_EQ(1u, framework.values.count("offers"));
  ASSERT_TRUE(framework.values.at("offers").is<JSON::Array>());
  ASSERT_EQ(1u, framework.values.at("offers").as<JSON::Array>().values.size());

  JSON::Object offer = framework.values.at("offers")
    .as<JSON::Array>().values[0].as<JSON::Object>();

  JSON::Object allocationInfo;
  allocationInfo.values["role"] = frameworkInfo.roles(0);

  ASSERT_EQ(1u, offer.values.count("allocation_info"));
  EXPECT_EQ(allocationInfo, offer.values.at("allocation_info"));

  driver.stop();
  driver.join();
}


TEST_F(MasterTest, StateSummaryEndpoint)
{
  master::Flags flags = CreateMasterFlags();

  flags.hostname = "localhost";
  flags.cluster = "test-cluster";

  Try<Owned<cluster::Master>> master = StartMaster(flags);
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), &containerizer);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  TaskID taskId;
  taskId.set_value("1");

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->MergeFrom(taskId);
  task.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task.mutable_resources()->MergeFrom(offers.get()[0].resources());
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  EXPECT_CALL(exec, registered(_, _, _, _));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status->state());

  EXPECT_CALL(exec, killTask(_, _))
    .WillOnce(SendStatusUpdateFromTaskID(TASK_KILLED));

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.killTask(taskId);

  AWAIT_READY(status);
  EXPECT_EQ(TASK_KILLED, status->state());

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  Future<Response> response = process::http::get(
      master.get()->pid,
      "state-summary",
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

  Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
  ASSERT_SOME(parse);

  JSON::Object state = parse.get();

  EXPECT_EQ(flags.hostname.get(), state.values["hostname"]);

  EXPECT_EQ(flags.cluster.get(), state.values["cluster"]);

  ASSERT_TRUE(state.values["slaves"].is<JSON::Array>());
  ASSERT_EQ(1u, state.values["slaves"].as<JSON::Array>().values.size());
  ASSERT_SOME_EQ(0u, state.find<JSON::Number>("slaves[0].TASK_RUNNING"));
  ASSERT_SOME_EQ(1u, state.find<JSON::Number>("slaves[0].TASK_KILLED"));

  ASSERT_TRUE(state.values["frameworks"].is<JSON::Array>());
  ASSERT_EQ(1u, state.values["frameworks"].as<JSON::Array>().values.size());
  ASSERT_SOME_EQ(0u, state.find<JSON::Number>("frameworks[0].TASK_RUNNING"));
  ASSERT_SOME_EQ(1u, state.find<JSON::Number>("frameworks[0].TASK_KILLED"));

  driver.stop();
  driver.join();
}


// This ensures that agent capabilities are included in
// the response of master's /state endpoint.
TEST_F(MasterTest, StateEndpointAgentCapabilities)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), master.get()->pid, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);

  Future<Response> response = process::http::get(
      master.get()->pid,
      "state",
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
  ASSERT_SOME(parse);

  Result<JSON::Array> slaveArray = parse->find<JSON::Array>("slaves");
  ASSERT_SOME(slaveArray);
  ASSERT_EQ(1u, slaveArray->values.size());

  JSON::Object slaveInfo = slaveArray->values[0].as<JSON::Object>();

  ASSERT_EQ(1u, slaveInfo.values.count("capabilities"));
  JSON::Value slaveCapabilities = slaveInfo.values.at("capabilities");

  // Agents should always have MULTI_ROLE, HIERARCHICAL_ROLE, and
  // RESERVATION_REFINEMENT capabilities in current implementation.
  Try<JSON::Value> expectedCapabilities = JSON::parse(
      "[\"MULTI_ROLE\",\"HIERARCHICAL_ROLE\",\"RESERVATION_REFINEMENT\"]");

  ASSERT_SOME(expectedCapabilities);
  EXPECT_TRUE(slaveCapabilities.contains(expectedCapabilities.get()));
}


// This ensures allocation role of task and its executor is exposed
// in master's /state endpoint.
TEST_F(MasterTest, StateEndpointAllocationRole)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), &containerizer);
  ASSERT_SOME(slave);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_roles(0, "foo");

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<Nothing> registered;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureSatisfy(&registered));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(registered);
  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  Resources executorResources = Resources::parse("cpus:0.1;mem:32").get();
  executorResources.allocate("foo");

  TaskID taskId;
  taskId.set_value("1");

  TaskInfo taskInfo;
  taskInfo.set_name("");
  taskInfo.mutable_task_id()->MergeFrom(taskId);
  taskInfo.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  taskInfo.mutable_resources()->MergeFrom(
      Resources(offers.get()[0].resources()) - executorResources);

  taskInfo.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);
  taskInfo.mutable_executor()->mutable_resources()->CopyFrom(executorResources);

  EXPECT_CALL(exec, registered(_, _, _, _));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.launchTasks(offers.get()[0].id(), {taskInfo});

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status->state());

  Future<Response> response = process::http::get(
      master.get()->pid,
      "state",
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
  ASSERT_SOME(parse);

  JSON::Value result = parse.get();

  JSON::Object expected = {
    {
      "frameworks",
      JSON::Array {
        JSON::Object {
          { "executors", JSON::Array {
            JSON::Object { { "role", frameworkInfo.roles(0) } } }
          },
          { "tasks", JSON::Array {
            JSON::Object { { "role", frameworkInfo.roles(0) } } }
          }
        }
      }
    }
  };

  EXPECT_TRUE(result.contains(expected));

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();
}


// This test verifies that recovered but yet to reregister agents are returned
// in `recovered_slaves` field of `/state` and `/slaves` endpoints.
TEST_F(MasterTest, RecoveredSlaves)
{
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.registry = "replicated_log";

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), master.get()->pid, _);

  // Reuse slaveFlags so both StartSlave() use the same work_dir.
  slave::Flags slaveFlags = this->CreateSlaveFlags();

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);

  SlaveID slaveID = slaveRegisteredMessage->slave_id();

  // Stop the slave while the master is down.
  master->reset();
  slave.get()->terminate();
  slave->reset();

  // Restart the master.
  master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Ensure that the agent is present in `recovered_slaves` field
  // while `slaves` field is empty in both `/state` and `/slaves`
  // endpoints.

  {
    Future<Response> response = process::http::get(
        master.get()->pid,
        "state",
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
    AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);

    Result<JSON::Array> array1 = parse->find<JSON::Array>("slaves");
    ASSERT_SOME(array1);
    EXPECT_TRUE(array1->values.empty());

    Result<JSON::Array> array2 =
      parse->find<JSON::Array>("recovered_slaves");

    ASSERT_SOME(array2);
    EXPECT_EQ(1u, array2->values.size());
  }

  {
    Future<Response> response = process::http::get(
        master.get()->pid,
        "slaves",
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);;
    AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);

    Result<JSON::Array> array1 = parse->find<JSON::Array>("slaves");
    ASSERT_SOME(array1);
    EXPECT_TRUE(array1->values.empty());

    Result<JSON::Array> array2 =
      parse->find<JSON::Array>("recovered_slaves");

    ASSERT_SOME(array2);
    EXPECT_EQ(1u, array2->values.size());
  }

  Future<SlaveReregisteredMessage> slaveReregisteredMessage =
    FUTURE_PROTOBUF(SlaveReregisteredMessage(), master.get()->pid, _);

  // Start the agent to make it re-register with the master.
  detector = master.get()->createDetector();
  slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  AWAIT_READY(slaveReregisteredMessage);

  // After the agent has successfully re-registered with the master, the
  // `recovered_slaves` field would be empty in both `/state` and `slave`
  // endpoints.

  {
    Future<Response> response1 = process::http::get(
        master.get()->pid,
        "state",
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response1);
    AWAIT_EXPECT_RESPONSE_HEADER_EQ(
        APPLICATION_JSON, "Content-Type", response1);

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response1->body);
    Result<JSON::Array> array1 = parse->find<JSON::Array>("slaves");
    ASSERT_SOME(array1);
    EXPECT_EQ(1u, array1->values.size());

    Result<JSON::Array> array2 =
      parse->find<JSON::Array>("recovered_slaves");
    ASSERT_SOME(array2);
    EXPECT_TRUE(array2->values.empty());
  }

  {
    Future<Response> response1 = process::http::get(
        master.get()->pid,
        "slaves",
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response1);
    AWAIT_EXPECT_RESPONSE_HEADER_EQ(
        APPLICATION_JSON, "Content-Type", response1);

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response1->body);
    Result<JSON::Array> array1 = parse->find<JSON::Array>("slaves");
    ASSERT_SOME(array1);
    EXPECT_EQ(1u, array1->values.size());

    Result<JSON::Array> array2 =
      parse->find<JSON::Array>("recovered_slaves");
    ASSERT_SOME(array2);
    EXPECT_TRUE(array2->values.empty());
  }
}


// This test verifies that executor labels are
// exposed in the master's state endpoint.
TEST_F(MasterTest, ExecutorLabels)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), &containerizer);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task.mutable_resources()->MergeFrom(offers.get()[0].resources());
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  // Add three labels to the executor, two of which shares the same key.
  Labels* labels = task.mutable_executor()->mutable_labels();

  labels->add_labels()->CopyFrom(createLabel("key1", "value1"));
  labels->add_labels()->CopyFrom(createLabel("key2", "value2"));
  labels->add_labels()->CopyFrom(createLabel("key1", "value3"));

  EXPECT_CALL(exec, registered(_, _, _, _));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status->state());

  // Verify label key and value in the master's state endpoint.
  Future<Response> response = process::http::get(
      master.get()->pid,
      "state",
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

  Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
  ASSERT_SOME(parse);

  Result<JSON::Array> labels_ = parse->find<JSON::Array>(
      "frameworks[0].executors[0].labels");
  EXPECT_SOME(labels_);

  // Verify the contents of labels.
  ASSERT_EQ(3u, labels_->values.size());
  EXPECT_EQ(JSON::Value(JSON::protobuf(createLabel("key1", "value1"))),
            labels_->values[0]);
  EXPECT_EQ(JSON::Value(JSON::protobuf(createLabel("key2", "value2"))),
            labels_->values[1]);
  EXPECT_EQ(JSON::Value(JSON::protobuf(createLabel("key1", "value3"))),
            labels_->values[2]);

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();
}


// This test verifies that label values are exposed over the master's
// state endpoint.
TEST_F(MasterTest, TaskLabels)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), &containerizer);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task.mutable_resources()->MergeFrom(offers.get()[0].resources());
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  // Add three labels to the task (two of which share the same key).
  Labels* labels = task.mutable_labels();

  labels->add_labels()->CopyFrom(createLabel("foo", "bar"));
  labels->add_labels()->CopyFrom(createLabel("bar", "baz"));
  labels->add_labels()->CopyFrom(createLabel("bar", "qux"));

  EXPECT_CALL(exec, registered(_, _, _, _));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<Nothing> update;
  EXPECT_CALL(containerizer,
              update(_, Resources(offers.get()[0].resources())))
    .WillOnce(DoAll(FutureSatisfy(&update),
                    Return(Nothing())));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status->state());

  AWAIT_READY(update);

  // Verify label key and value in the master's state endpoint.
  Future<Response> response = process::http::get(
      master.get()->pid,
      "state",
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

  Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
  ASSERT_SOME(parse);

  Result<JSON::Array> find = parse->find<JSON::Array>(
      "frameworks[0].tasks[0].labels");
  EXPECT_SOME(find);

  JSON::Array labelsObject = find.get();

  // Verify the contents of 'foo:bar', 'bar:baz', and 'bar:qux' pairs.
  EXPECT_EQ(
      JSON::Value(JSON::protobuf(createLabel("foo", "bar"))),
      labelsObject.values[0]);
  EXPECT_EQ(
      JSON::Value(JSON::protobuf(createLabel("bar", "baz"))),
      labelsObject.values[1]);
  EXPECT_EQ(
      JSON::Value(JSON::protobuf(createLabel("bar", "qux"))),
      labelsObject.values[2]);

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();
}


// This test verifies that TaskStatus label values are exposed over
// the master's state endpoint.
TEST_F(MasterTest, TaskStatusLabels)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), &containerizer);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  TaskInfo task = createTask(offers.get()[0], "sleep 100", DEFAULT_EXECUTOR_ID);

  ExecutorDriver* execDriver;
  EXPECT_CALL(exec, registered(_, _, _, _))
    .WillOnce(SaveArg<0>(&execDriver));

  Future<TaskInfo> execTask;
  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(FutureArg<1>(&execTask));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(execTask);

  // Now send TASK_RUNNING update.
  TaskStatus runningStatus;
  runningStatus.mutable_task_id()->MergeFrom(execTask->task_id());
  runningStatus.set_state(TASK_RUNNING);

  // Add three labels to the task (two of which share the same key).
  Labels* labels = runningStatus.mutable_labels();

  labels->add_labels()->CopyFrom(createLabel("foo", "bar"));
  labels->add_labels()->CopyFrom(createLabel("bar", "baz"));
  labels->add_labels()->CopyFrom(createLabel("bar", "qux"));

  execDriver->sendStatusUpdate(runningStatus);

  AWAIT_READY(status);

  // Verify label key and value in master state endpoint.
  Future<Response> response = process::http::get(
      master.get()->pid,
      "state",
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

  Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
  ASSERT_SOME(parse);

  Result<JSON::Array> find = parse->find<JSON::Array>(
      "frameworks[0].tasks[0].statuses[0].labels");
  EXPECT_SOME(find);

  JSON::Array labelsObject = find.get();

  // Verify the content of 'foo:bar' pair.
  EXPECT_EQ(
      JSON::Value(JSON::protobuf(createLabel("foo", "bar"))),
      labelsObject.values[0]);
  EXPECT_EQ(
      JSON::Value(JSON::protobuf(createLabel("bar", "baz"))),
      labelsObject.values[1]);
  EXPECT_EQ(
      JSON::Value(JSON::protobuf(createLabel("bar", "qux"))),
      labelsObject.values[2]);

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();
}


// This test verifies that TaskStatus::container_status is exposed over the
// master state endpoint.
TEST_F(MasterTest, TaskStatusContainerStatus)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), &containerizer);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  TaskInfo task = createTask(offers.get()[0], "sleep 100", DEFAULT_EXECUTOR_ID);

  ExecutorDriver* execDriver;
  EXPECT_CALL(exec, registered(_, _, _, _))
    .WillOnce(SaveArg<0>(&execDriver));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(status);

  const string slaveIPAddress = stringify(slave.get()->pid.address.ip);

  // Validate that the Slave has passed in its IP address in
  // TaskStatus.container_status.network_infos[0].ip_address.
  EXPECT_TRUE(status->has_container_status());
  ContainerStatus containerStatus = status->container_status();
  ASSERT_EQ(1, containerStatus.network_infos().size());
  ASSERT_EQ(1, containerStatus.network_infos(0).ip_addresses().size());

  NetworkInfo::IPAddress ipAddress =
    containerStatus.network_infos(0).ip_addresses(0);

  ASSERT_TRUE(ipAddress.has_ip_address());
  EXPECT_EQ(slaveIPAddress, ipAddress.ip_address());

  // Now do the same validation with state endpoint.
  Future<Response> response = process::http::get(
      master.get()->pid,
      "state",
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

  Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
  ASSERT_SOME(parse);

  // Validate that the IP address passed in by the Slave is available at the
  // state endpoint.
  ASSERT_SOME_EQ(
      slaveIPAddress,
      parse->find<JSON::String>(
          "frameworks[0].tasks[0].statuses[0]"
          ".container_status.network_infos[0]"
          ".ip_addresses[0].ip_address"));

  // Now test for explicit reconciliation.
  Future<TaskStatus> explicitReconciliationStatus;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&explicitReconciliationStatus));

  // Send a task status to trigger explicit reconciliation.
  TaskStatus taskStatus;
  taskStatus.mutable_task_id()->CopyFrom(status->task_id());
  // State is not checked by reconciliation, but is required to be
  // a valid task status.
  taskStatus.set_state(TASK_RUNNING);
  driver.reconcileTasks({taskStatus});

  AWAIT_READY(explicitReconciliationStatus);
  EXPECT_EQ(TASK_RUNNING, explicitReconciliationStatus->state());
  EXPECT_TRUE(explicitReconciliationStatus->has_container_status());

  containerStatus = explicitReconciliationStatus->container_status();
  ASSERT_EQ(1, containerStatus.network_infos().size());
  ASSERT_EQ(1, containerStatus.network_infos(0).ip_addresses().size());

  ipAddress = containerStatus.network_infos(0).ip_addresses(0);

  ASSERT_TRUE(ipAddress.has_ip_address());
  EXPECT_EQ(slaveIPAddress, ipAddress.ip_address());

  Future<TaskStatus> implicitReconciliationStatus;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&implicitReconciliationStatus));

  // Send an empty vector of task statuses to trigger implicit reconciliation.
  driver.reconcileTasks({});

  AWAIT_READY(implicitReconciliationStatus);
  EXPECT_EQ(TASK_RUNNING, implicitReconciliationStatus->state());
  ASSERT_TRUE(implicitReconciliationStatus->has_container_status());

  containerStatus = implicitReconciliationStatus->container_status();
  ASSERT_EQ(1, containerStatus.network_infos().size());
  ASSERT_EQ(1, containerStatus.network_infos(0).ip_addresses().size());

  ipAddress = containerStatus.network_infos(0).ip_addresses(0);

  ASSERT_TRUE(ipAddress.has_ip_address());
  EXPECT_EQ(slaveIPAddress, ipAddress.ip_address());

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();
}


// This tests the 'active' field in slave entries from the master's
// state endpoint. We first verify an active slave, deactivate it
// and verify that the 'active' field is false.
TEST_F(MasterTest, SlaveActiveEndpoint)
{
  // Start a master.
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Future<process::Message> slaveRegisteredMessage =
    FUTURE_MESSAGE(Eq(SlaveRegisteredMessage().GetTypeName()), _, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);

  // Verify slave is active.
  Future<Response> response = process::http::get(
      master.get()->pid,
      "state",
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

  Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
  ASSERT_SOME(parse);

  Result<JSON::Boolean> status = parse->find<JSON::Boolean>(
      "slaves[0].active");

  ASSERT_SOME_EQ(JSON::Boolean(true), status);

  Future<Nothing> deactivateSlave =
    FUTURE_DISPATCH(_, &MesosAllocatorProcess::deactivateSlave);

  // Inject a slave exited event at the master causing the master
  // to mark the slave as disconnected.
  process::inject::exited(slaveRegisteredMessage->to, master.get()->pid);

  // Wait until master deactivates the slave.
  AWAIT_READY(deactivateSlave);

  // Verify slave is inactive.
  response = process::http::get(
      master.get()->pid,
      "state",
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  parse = JSON::parse<JSON::Object>(response->body);
  ASSERT_SOME(parse);

  status = parse->find<JSON::Boolean>("slaves[0].active");

  ASSERT_SOME_EQ(JSON::Boolean(false), status);
}


// This test verifies that service info for tasks is exposed over the
// master's state endpoint.
TEST_F(MasterTest, TaskDiscoveryInfo)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), &containerizer);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  TaskInfo task;
  task.set_name("testtask");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->CopyFrom(offers.get()[0].slave_id());
  task.mutable_resources()->CopyFrom(offers.get()[0].resources());
  task.mutable_executor()->CopyFrom(DEFAULT_EXECUTOR_INFO);

  // An expanded service discovery info to the task.
  DiscoveryInfo* info = task.mutable_discovery();
  info->set_visibility(DiscoveryInfo::EXTERNAL);
  info->set_name("mytask");
  info->set_environment("mytest");
  info->set_location("mylocation");
  info->set_version("v0.1.1");

  // Add two named ports to the discovery info.
  Ports* ports = info->mutable_ports();
  Port* port1 = ports->add_ports();
  port1->set_number(8888);
  port1->set_name("myport1");
  port1->set_protocol("tcp");
  Port* port2 = ports->add_ports();
  port2->set_number(9999);
  port2->set_name("myport2");
  port2->set_protocol("udp");
  port2->set_visibility(DiscoveryInfo::CLUSTER);

  // Add two labels to the discovery info.
  Labels* labels = info->mutable_labels();
  labels->add_labels()->CopyFrom(createLabel("clearance", "high"));
  labels->add_labels()->CopyFrom(createLabel("RPC", "yes"));

  EXPECT_CALL(exec, registered(_, _, _, _));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<Nothing> update;
  EXPECT_CALL(containerizer,
              update(_, Resources(offers.get()[0].resources())))
    .WillOnce(DoAll(FutureSatisfy(&update),
                    Return(Nothing())));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status->state());

  AWAIT_READY(update);

  // Verify label key and value in the master's state endpoint.
  Future<Response> response = process::http::get(
      master.get()->pid,
      "state",
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

  Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
  ASSERT_SOME(parse);

  Result<JSON::String> taskName = parse->find<JSON::String>(
      "frameworks[0].tasks[0].name");
  ASSERT_SOME(taskName);
  ASSERT_EQ("testtask", taskName.get());

  // Verify basic content for discovery info.
  Result<JSON::String> visibility = parse->find<JSON::String>(
      "frameworks[0].tasks[0].discovery.visibility");
  EXPECT_SOME(visibility);
  DiscoveryInfo::Visibility visibility_value;
  DiscoveryInfo::Visibility_Parse(visibility->value, &visibility_value);
  ASSERT_EQ(DiscoveryInfo::EXTERNAL, visibility_value);

  Result<JSON::String> discoveryName = parse->find<JSON::String>(
      "frameworks[0].tasks[0].discovery.name");
  ASSERT_SOME(discoveryName);
  ASSERT_EQ("mytask", discoveryName.get());

  Result<JSON::String> environment = parse->find<JSON::String>(
      "frameworks[0].tasks[0].discovery.environment");
  ASSERT_SOME(environment);
  ASSERT_EQ("mytest", environment.get());

  Result<JSON::String> location = parse->find<JSON::String>(
      "frameworks[0].tasks[0].discovery.location");
  ASSERT_SOME(location);
  ASSERT_EQ("mylocation", location.get());

  Result<JSON::String> version = parse->find<JSON::String>(
      "frameworks[0].tasks[0].discovery.version");
  ASSERT_SOME(version);
  ASSERT_EQ("v0.1.1", version.get());

  // Verify content of two named ports.
  Result<JSON::Array> find1 = parse->find<JSON::Array>(
      "frameworks[0].tasks[0].discovery.ports.ports");
  ASSERT_SOME(find1);

  JSON::Array portsArray = find1.get();
  ASSERT_EQ(2u, portsArray.values.size());

  // Verify the content of '8888:myport1:tcp' port.
  Try<JSON::Value> expected = JSON::parse(
      "{"
      "  \"number\":8888,"
      "  \"name\":\"myport1\","
      "  \"protocol\":\"tcp\""
      "}");
  ASSERT_SOME(expected);
  EXPECT_EQ(expected.get(), portsArray.values[0]);

  // Verify the content of '9999:myport2:udp' port.
  expected = JSON::parse(
      "{"
      "  \"number\":9999,"
      "  \"name\":\"myport2\","
      "  \"protocol\":\"udp\","
      "  \"visibility\":\"CLUSTER\""
      "}");
  ASSERT_SOME(expected);
  EXPECT_EQ(expected.get(), portsArray.values[1]);

  // Verify content of two labels.
  Result<JSON::Array> find2 = parse->find<JSON::Array>(
      "frameworks[0].tasks[0].discovery.labels.labels");
  EXPECT_SOME(find2);

  JSON::Array labelsArray = find2.get();
  ASSERT_EQ(2u, labelsArray.values.size());

  // Verify the content of 'clearance:high' pair.
  EXPECT_EQ(
      JSON::Value(JSON::protobuf(createLabel("clearance", "high"))),
      labelsArray.values[0]);

  // Verify the content of 'RPC:yes' pair.
  EXPECT_EQ(
      JSON::Value(JSON::protobuf(createLabel("RPC", "yes"))),
      labelsArray.values[1]);

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();
}


// Test verifies that a long lived executor works after master
// fail-over. The test launches a task, restarts the master and
// launches another task using the same executor.
TEST_F(MasterTest, MasterFailoverLongLivedExecutor)
{
  // Start master and create detector to inform scheduler and slave
  // about newly elected master.
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  // Compute half of total available resources in order to launch two
  // tasks on the same executor (and thus slave).
  Resources halfSlave = Resources::parse("cpus:1;mem:512").get();
  Resources fullSlave = halfSlave + halfSlave;

  slave::Flags flags = CreateSlaveFlags();
  flags.resources = Option<string>(stringify(fullSlave));

  StandaloneMasterDetector detector(master.get()->pid);

  Try<Owned<cluster::Slave>> slave =
    StartSlave(&detector, &containerizer, flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  TestingMesosSchedulerDriver driver(&sched, &detector);

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(2);

  EXPECT_CALL(sched, disconnected(&driver));

  Future<vector<Offer>> offers1;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers1))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers1);
  ASSERT_FALSE(offers1->empty());

  TaskInfo task1;
  task1.set_name("");
  task1.mutable_task_id()->set_value("1");
  task1.mutable_slave_id()->MergeFrom(offers1.get()[0].slave_id());
  task1.mutable_resources()->MergeFrom(halfSlave);
  task1.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  EXPECT_CALL(exec, registered(_, _, _, _));

  // Expect two tasks to eventually be running on the executor.
  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> status1;
  EXPECT_CALL(sched, statusUpdate(&driver, TaskStatusTaskIdEq(task1)))
    .WillOnce(FutureArg<1>(&status1))
    .WillRepeatedly(Return());

  driver.launchTasks(offers1.get()[0].id(), {task1});

  AWAIT_READY(status1);
  EXPECT_EQ(TASK_RUNNING, status1->state());

  // Fail over master.
  master->reset();
  master = StartMaster();
  ASSERT_SOME(master);

  // Subsequent offers have been ignored until now, set an expectation
  // to get offers from the failed over master.
  Future<vector<Offer>> offers2;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers2))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  detector.appoint(master.get()->pid);

  AWAIT_READY(offers2);
  ASSERT_FALSE(offers2->empty());

  // The second task is a just a copy of the first task (using the
  // same executor and resources). We have to set a new task id.
  TaskInfo task2 = task1;
  task2.mutable_task_id()->set_value("2");

  Future<TaskStatus> status2;
  EXPECT_CALL(sched, statusUpdate(&driver, TaskStatusTaskIdEq(task2)))
    .WillOnce(FutureArg<1>(&status2))
    .WillRepeatedly(Return());

  // Start the second task with the new master on the running executor.
  driver.launchTasks(offers2.get()[0].id(), {task2});

  AWAIT_READY(status2);
  EXPECT_EQ(TASK_RUNNING, status2->state());

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();
}


// This test ensures that a slave gets a unique SlaveID even after
// master fails over. Please refer to MESOS-3351 for further details.
TEST_F(MasterTest, DuplicatedSlaveIdWhenSlaveReregister)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage1 =
      FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  StandaloneMasterDetector slaveDetector1(master.get()->pid);
  Try<Owned<cluster::Slave>> slave1 = StartSlave(&slaveDetector1);
  ASSERT_SOME(slave1);

  AWAIT_READY(slaveRegisteredMessage1);

  // Fail over master.
  master->reset();
  master = StartMaster();
  ASSERT_SOME(master);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage2 =
      FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  // Start a new slave and make sure it registers before the old slave.
  slave::Flags slaveFlags2 = CreateSlaveFlags();
  Owned<MasterDetector> slaveDetector2 = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave2 =
    StartSlave(slaveDetector2.get(), slaveFlags2);
  ASSERT_SOME(slave2);

  AWAIT_READY(slaveRegisteredMessage2);

  Future<SlaveReregisteredMessage> slaveReregisteredMessage1 =
      FUTURE_PROTOBUF(SlaveReregisteredMessage(), master.get()->pid, _);

  // Now let the first slave re-register.
  slaveDetector1.appoint(master.get()->pid);

  // If both the slaves get the same SlaveID, the re-registration would
  // fail here.
  AWAIT_READY(slaveReregisteredMessage1);
}


// This test ensures that if a framework scheduler provides any
// labels in its FrameworkInfo message, those labels are included
// in the master's state endpoint.
TEST_F(MasterTest, FrameworkInfoLabels)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  FrameworkInfo framework = DEFAULT_FRAMEWORK_INFO;

  // Add three labels to the FrameworkInfo. Two labels share the same key.
  framework.mutable_labels()->add_labels()->CopyFrom(createLabel("foo", "bar"));
  framework.mutable_labels()->add_labels()->CopyFrom(createLabel("bar", "baz"));
  framework.mutable_labels()->add_labels()->CopyFrom(createLabel("bar", "qux"));

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, framework, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<Nothing> registered;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureSatisfy(&registered));

  driver.start();

  AWAIT_READY(registered);

  Future<Response> response = process::http::get(
      master.get()->pid,
      "state",
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

  Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
  ASSERT_SOME(parse);

  Result<JSON::Array> labelsObject = parse->find<JSON::Array>(
      "frameworks[0].labels");
  ASSERT_SOME(labelsObject);

  JSON::Array labelsObject_ = labelsObject.get();

  EXPECT_EQ(
      JSON::Value(JSON::protobuf(createLabel("foo", "bar"))),
      labelsObject_.values[0]);

  EXPECT_EQ(
      JSON::Value(JSON::protobuf(createLabel("bar", "baz"))),
      labelsObject_.values[1]);

  EXPECT_EQ(
      JSON::Value(JSON::protobuf(createLabel("bar", "qux"))),
      labelsObject_.values[2]);

  driver.stop();
  driver.join();
}


// This test ensures that if a framework scheduler provides invalid
// role in its FrameworkInfo message, the master will reject it.
TEST_F(MasterTest, RejectFrameworkWithInvalidRole)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  FrameworkInfo framework = DEFAULT_FRAMEWORK_INFO;

  // Add invalid role to the FrameworkInfo.
  framework.set_roles(0, "/test/test1");

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, framework, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<string> error;
  EXPECT_CALL(sched, error(&driver, _))
    .WillOnce(FutureArg<1>(&error));

  driver.start();

  AWAIT_READY(error);
}


TEST_F(MasterTest, FrameworksEndpointWithoutFrameworks)
{
  master::Flags flags = CreateMasterFlags();

  flags.hostname = "localhost";
  flags.cluster = "test-cluster";

  // Capture the start time deterministically.
  Clock::pause();

  Try<Owned<cluster::Master>> master = StartMaster(flags);
  ASSERT_SOME(master);

  Future<Response> response = process::http::get(
      master.get()->pid,
      "frameworks",
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

  Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
  ASSERT_SOME(parse);

  JSON::Object frameworks = parse.get();

  ASSERT_TRUE(frameworks.values["frameworks"].is<JSON::Array>());
  EXPECT_TRUE(frameworks.values["frameworks"].as<JSON::Array>().values.empty());

  ASSERT_TRUE(
      frameworks.values["completed_frameworks"].is<JSON::Array>());
  EXPECT_TRUE(
      frameworks.values["completed_frameworks"].as<JSON::Array>().values
      .empty());

  ASSERT_TRUE(
      frameworks.values["unregistered_frameworks"].is<JSON::Array>());
  EXPECT_TRUE(
      frameworks.values["unregistered_frameworks"].as<JSON::Array>().values
      .empty());
}


// Ensures that the '/master/frameworks' endpoint returns the correct framework
// when provided with a framework ID query parameter.
TEST_F(MasterTest, FrameworksEndpointMultipleFrameworks)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Start a slave to receive shutdown message when framework is terminated.
  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  Future<RegisterSlaveMessage> registerSlaveMessage =
    FUTURE_PROTOBUF(RegisterSlaveMessage(), _, _);

  AWAIT_READY(registerSlaveMessage);

  // Start two frameworks.

  Future<FrameworkID> frameworkId1;
  Future<FrameworkID> frameworkId2;

  MockScheduler sched1;
  MesosSchedulerDriver driver1(
      &sched1,
      DEFAULT_FRAMEWORK_INFO,
      master.get()->pid,
      DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched1, registered(_, _, _))
    .WillOnce(FutureArg<1>(&frameworkId1));

  // Ignore any incoming resource offers to the scheduler.
  EXPECT_CALL(sched1, resourceOffers(_, _))
    .WillRepeatedly(Return());

  driver1.start();

  MockScheduler sched2;
  MesosSchedulerDriver driver2(
      &sched2,
      DEFAULT_FRAMEWORK_INFO,
      master.get()->pid,
      DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched2, registered(_, _, _))
    .WillOnce(FutureArg<1>(&frameworkId2));

  // Ignore any incoming resource offers to the scheduler.
  EXPECT_CALL(sched2, resourceOffers(_, _))
    .WillRepeatedly(Return());

  driver2.start();

  AWAIT_READY(frameworkId1);
  AWAIT_READY(frameworkId2);

  // Request with no query parameter.
  {
    Future<Response> response = process::http::get(
        master.get()->pid,
        "frameworks",
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
    AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

    Try<JSON::Value> value = JSON::parse<JSON::Value>(response->body);
    ASSERT_SOME(value);

    JSON::Object object = value->as<JSON::Object>();

    Result<JSON::Array> array = object.find<JSON::Array>("frameworks");
    ASSERT_SOME(array);
    EXPECT_EQ(2u, array->values.size());

    Try<JSON::Value> frameworkJson1 = JSON::parse(
        "{"
            "\"id\":\"" + frameworkId1->value() + "\","
            "\"name\":\"default\""
        "}");

    Try<JSON::Value> frameworkJson2 = JSON::parse(
        "{"
            "\"id\":\"" + frameworkId2->value() + "\","
            "\"name\":\"default\""
        "}");

    ASSERT_SOME(frameworkJson1);
    ASSERT_SOME(frameworkJson2);

    // Since frameworks are stored in a hashmap, there is no strict guarantee of
    // their ordering when listed. For this reason, we test both possibilities.
    if (array->values[0].contains(frameworkJson1.get())) {
      ASSERT_TRUE(array->values[1].contains(frameworkJson2.get()));
    } else {
      ASSERT_TRUE(array->values[0].contains(frameworkJson2.get()));
      ASSERT_TRUE(array->values[1].contains(frameworkJson1.get()));
    }
  }

  // Query the first framework.
  {
    Future<Response> response = process::http::get(
        master.get()->pid,
        "frameworks?framework_id=" + frameworkId1->value(),
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
    AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

    Try<JSON::Value> value = JSON::parse<JSON::Value>(response->body);
    ASSERT_SOME(value);

    JSON::Object object = value->as<JSON::Object>();

    Result<JSON::Array> array = object.find<JSON::Array>("frameworks");
    ASSERT_SOME(array);
    EXPECT_EQ(1u, array->values.size());

    Try<JSON::Value> expected = JSON::parse(
        "{"
          "\"frameworks\":"
            "[{"
                "\"id\":\"" + frameworkId1->value() + "\","
                "\"name\":\"default\""
            "}]"
        "}");

    ASSERT_SOME(expected);

    EXPECT_TRUE(value->contains(expected.get()));
  }

  // Expect a teardown call and a shutdown message to ensure that the
  // master has marked the framework as completed.
  Future<mesos::scheduler::Call> teardownCall = FUTURE_CALL(
      mesos::scheduler::Call(), mesos::scheduler::Call::TEARDOWN, _, _);
  Future<ShutdownFrameworkMessage> shutdownFrameworkMessage =
    FUTURE_PROTOBUF(ShutdownFrameworkMessage(), _, _);

  // Complete the first framework. As a result, it will appear in the response's
  // 'completed_frameworks' field.
  driver1.stop();
  driver1.join();

  AWAIT_READY(teardownCall);

  AWAIT_READY(shutdownFrameworkMessage);

  // Query the first framework.
  {
    Future<Response> response = process::http::get(
        master.get()->pid,
        "frameworks?framework_id=" + frameworkId1->value(),
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
    AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

    Try<JSON::Value> value = JSON::parse<JSON::Value>(response->body);
    ASSERT_SOME(value);

    JSON::Object object = value->as<JSON::Object>();

    Result<JSON::Array> array =
      object.find<JSON::Array>("completed_frameworks");
    ASSERT_SOME(array);
    EXPECT_EQ(1u, array->values.size());

    Try<JSON::Value> expected = JSON::parse(
        "{"
          "\"completed_frameworks\":"
            "[{"
                "\"id\":\"" + frameworkId1->value() + "\","
                "\"name\":\"default\""
            "}]"
        "}");

    ASSERT_SOME(expected);

    EXPECT_TRUE(value->contains(expected.get()));
  }

  driver2.stop();
  driver2.join();
}


// Test the max_completed_frameworks flag for master.
TEST_F(MasterTest, MaxCompletedFrameworksFlag)
{
  // In order to verify that the proper amount of history
  // is maintained, we launch exactly 2 frameworks when
  // 'max_completed_frameworks' is set to 0, 1, and 2. This
  // covers the cases of maintaining no history, some history
  // less than the total number of frameworks launched, and
  // history equal to the total number of frameworks launched.
  const size_t totalFrameworks = 2;
  const size_t maxFrameworksArray[] = {0, 1, 2};

  foreach (const size_t maxFrameworks, maxFrameworksArray) {
    master::Flags masterFlags = CreateMasterFlags();
    masterFlags.max_completed_frameworks = maxFrameworks;

    Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
    ASSERT_SOME(master);

    Owned<MasterDetector> detector = master.get()->createDetector();
    Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
    ASSERT_SOME(slave);

    for (size_t i = 0; i < totalFrameworks; i++) {
      MockScheduler sched;
      MesosSchedulerDriver schedDriver(
          &sched,
          DEFAULT_FRAMEWORK_INFO,
          master.get()->pid,
          DEFAULT_CREDENTIAL);

      // Ignore any incoming resource offers to the scheduler.
      EXPECT_CALL(sched, resourceOffers(_, _))
        .WillRepeatedly(Return());

      Future<Nothing> schedRegistered;
      EXPECT_CALL(sched, registered(_, _, _))
        .WillOnce(FutureSatisfy(&schedRegistered));

      schedDriver.start();

      AWAIT_READY(schedRegistered);

      schedDriver.stop();
      schedDriver.join();
    }

    Future<Response> response = process::http::get(
        master.get()->pid,
        "state",
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
    ASSERT_SOME(parse);
    JSON::Object state = parse.get();

    // The number of completed frameworks should match the limit.
    Result<JSON::Array> completedFrameworks =
      state.values["completed_frameworks"].as<JSON::Array>();

    EXPECT_EQ(maxFrameworks, completedFrameworks->values.size());
  }
}


// Test the max_completed_tasks_per_framework flag for master.
TEST_F(MasterTest, MaxCompletedTasksPerFrameworkFlag)
{
  // We verify that the proper amount of history is maintained
  // by launching a single framework with exactly 2 tasks. We
  // do this when setting `max_completed_tasks_per_framework`
  // to 0, 1, and 2. This covers the cases of maintaining no
  // history, some history less than the total number of tasks
  // launched, and history equal to the total number of tasks
  // launched.
  const size_t totalTasksPerFramework = 2;
  const size_t maxTasksPerFrameworkArray[] = {0, 1, 2};

  Clock::pause();

  foreach (const size_t maxTasksPerFramework, maxTasksPerFrameworkArray) {
    master::Flags masterFlags = CreateMasterFlags();
    masterFlags.max_completed_tasks_per_framework = maxTasksPerFramework;

    Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
    ASSERT_SOME(master);

    MockExecutor exec(DEFAULT_EXECUTOR_ID);
    TestContainerizer containerizer(&exec);
    EXPECT_CALL(exec, registered(_, _, _, _));

    Future<SlaveRegisteredMessage> slaveRegisteredMessage =
      FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

    Owned<MasterDetector> detector = master.get()->createDetector();
    slave::Flags agentFlags = CreateSlaveFlags();
    Try<Owned<cluster::Slave>> slave =
      StartSlave(detector.get(), &containerizer, agentFlags);
    ASSERT_SOME(slave);

    Clock::advance(agentFlags.registration_backoff_factor);
    AWAIT_READY(slaveRegisteredMessage);

    MockScheduler sched;
    MesosSchedulerDriver schedDriver(
        &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

    Future<Nothing> schedRegistered;
    EXPECT_CALL(sched, registered(_, _, _))
      .WillOnce(FutureSatisfy(&schedRegistered));

    process::Queue<Offer> offers;
    EXPECT_CALL(sched, resourceOffers(_, _))
      .WillRepeatedly(EnqueueOffers(&offers));

    schedDriver.start();

    AWAIT_READY(schedRegistered);

    for (size_t i = 0; i < totalTasksPerFramework; i++) {
      // Trigger a batch allocation.
      Clock::advance(masterFlags.allocation_interval);

      Future<Offer> offer = offers.get();
      AWAIT_READY(offer);

      TaskInfo task;
      task.set_name("");
      task.mutable_task_id()->set_value(stringify(i));
      task.mutable_slave_id()->MergeFrom(offer->slave_id());
      task.mutable_resources()->MergeFrom(offer->resources());
      task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

      // Make sure the task passes through its TASK_FINISHED
      // state properly. We force this state change through
      // the launchTask() callback on our MockExecutor.
      Future<TaskStatus> statusFinished;
      EXPECT_CALL(exec, launchTask(_, _))
        .WillOnce(SendStatusUpdateFromTask(TASK_FINISHED));
      EXPECT_CALL(sched, statusUpdate(_, _))
        .WillOnce(FutureArg<1>(&statusFinished));

      schedDriver.launchTasks(offer->id(), {task});

      AWAIT_READY(statusFinished);
      EXPECT_EQ(TASK_FINISHED, statusFinished->state());
    }

    EXPECT_CALL(exec, shutdown(_))
      .Times(AtMost(1));

    schedDriver.stop();
    schedDriver.join();

    Future<Response> response = process::http::get(
        master.get()->pid,
        "state",
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
    ASSERT_SOME(parse);
    JSON::Object state = parse.get();

    // There should be only 1 completed framework.
    Result<JSON::Array> completedFrameworks =
      state.values["completed_frameworks"].as<JSON::Array>();

    ASSERT_EQ(1u, completedFrameworks->values.size());

    // The number of completed tasks in the completed framework
    // should match the limit.
    JSON::Object completedFramework =
      completedFrameworks->values[0].as<JSON::Object>();
    Result<JSON::Array> completedTasksPerFramework =
      completedFramework.values["completed_tasks"].as<JSON::Array>();

    EXPECT_EQ(maxTasksPerFramework, completedTasksPerFramework->values.size());
  }
}


// Test GET requests on various endpoints without authentication and
// with bad credentials.
// Note that we have similar checks for the maintenance, roles, quota, teardown,
// reserve, unreserve, create-volumes, destroy-volumes, observe endpoints in the
// respective test files.
TEST_F(MasterTest, EndpointsBadAuthentication)
{
  // Set up a master with authentication required.
  // Note that the default master test flags enable HTTP authentication.
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Bad credentials which should fail authentication.
  Credential badCredential;
  badCredential.set_principal("badPrincipal");
  badCredential.set_secret("badSecret");

  // frameworks endpoint.
  {
    // Get request without authentication.
    Future<Response> response = process::http::get(
        master.get()->pid,
        "frameworks");

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(Unauthorized({}).status, response);

    // Get request with bad authentication.
    response = process::http::get(
      master.get()->pid,
      "frameworks",
      None(),
      createBasicAuthHeaders(badCredential));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(Unauthorized({}).status, response);
  }

  // flags endpoint.
  {
    // Get request without authentication.
    Future<Response> response = process::http::get(master.get()->pid, "flags");

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(Unauthorized({}).status, response);

    // Get request with bad authentication.
    response = process::http::get(
      master.get()->pid,
      "flags",
      None(),
      createBasicAuthHeaders(badCredential));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(Unauthorized({}).status, response);
  }

  // slaves endpoint.
  {
    // Get request without authentication.
    Future<Response> response = process::http::get(master.get()->pid, "slaves");

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(Unauthorized({}).status, response);

    // Get request with bad authentication.
    response = process::http::get(
      master.get()->pid,
      "slaves",
      None(),
      createBasicAuthHeaders(badCredential));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(Unauthorized({}).status, response);
  }

  // state endpoint.
  {
    // Get request without authentication.
    Future<Response> response = process::http::get(master.get()->pid, "state");

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(Unauthorized({}).status, response);

    // Get request with bad authentication.
    response = process::http::get(
      master.get()->pid,
      "state",
      None(),
      createBasicAuthHeaders(badCredential));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(Unauthorized({}).status, response);
  }

  // stateSummary endpoint.
  {
    // Get request without authentication.
    Future<Response> response = process::http::get(
        master.get()->pid,
        "state-summary");

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(Unauthorized({}).status, response);

    // Get request with bad authentication.
    response = process::http::get(
      master.get()->pid,
      "state-summary",
      None(),
      createBasicAuthHeaders(badCredential));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(Unauthorized({}).status, response);
  }

  // tasks endpoint.
  {
    // Get request without authentication.
    Future<Response> response = process::http::get(master.get()->pid, "tasks");

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(Unauthorized({}).status, response);

    // Get request with bad authentication.
    response = process::http::get(
      master.get()->pid,
      "tasks",
      None(),
      createBasicAuthHeaders(badCredential));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(Unauthorized({}).status, response);
  }
}


// Test unauthenticated GET requests on various endpoints
// when authentication is disabled for read-only endpoints.
TEST_F(MasterTest, ReadonlyEndpointsNoAuthentication)
{
  // Set up a master with authentication disabled for read-only endpoints.
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.authenticate_http_readonly = false;

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // `state` endpoint from master should be allowed without authentication.
  {
    Future<Response> response = process::http::get(master.get()->pid, "state");

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  }

  // `quota` endpoint from master is controlled by `authenticate_http_readwrite`
  // flag which is set to true, so an unauthenticated request will be rejected.
  {
    Future<Response> response = process::http::get(master.get()->pid, "quota");

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(Unauthorized({}).status, response);
  }
}


// Test GET requests on various endpoints without authentication
// when authentication for read-write endpoints is disabled.
TEST_F(MasterTest, ReadwriteEndpointsNoAuthentication)
{
  // Set up a master with authentication disabled for read-write endpoints.
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.authenticate_http_readwrite = false;

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // `quota` endpoint from master should be allowed without authentication.
  {
    Future<Response> response = process::http::get(master.get()->pid, "quota");

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  }

  // `state` endpoint from master is controlled by `authenticate_http_readonly`
  // flag which is set to true, so an unauthenticated request will be rejected.
  {
    Future<Response> response = process::http::get(master.get()->pid, "state");

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(Unauthorized({}).status, response);
  }
}


TEST_F(MasterTest, RejectFrameworkWithInvalidFailoverTimeout)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  FrameworkInfo framework = DEFAULT_FRAMEWORK_INFO;

  // Add invalid failover timeout to the FrameworkInfo.
  // As the timeout is represented using nanoseconds as an int64, the
  // following value converted to seconds is too large and does not
  // fit in int64.
  framework.set_failover_timeout(99999999999999999);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, framework, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<string> error;
  EXPECT_CALL(sched, error(&driver, _))
    .WillOnce(FutureArg<1>(&error));

  driver.start();

  AWAIT_READY(error);
}


// This test verifies that we recover resources when an orphaned task reaches
// a terminal state.
TEST_F(MasterTest, DISABLED_RecoverResourcesOrphanedTask)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();
  auto executor = std::make_shared<v1::MockHTTPExecutor>();

  ExecutorID executorId = DEFAULT_EXECUTOR_ID;
  TestContainerizer containerizer(executorId, executor);

  StandaloneMasterDetector detector(master.get()->pid);
  Try<Owned<cluster::Slave>> slave = StartSlave(&detector, &containerizer);
  ASSERT_SOME(slave);

  Future<Nothing> connected;
  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(FutureSatisfy(&connected))
    .WillOnce(Return());

  ContentType contentType = ContentType::PROTOBUF;

  v1::scheduler::TestMesos mesos(
      master.get()->pid,
      contentType,
      scheduler);

  AWAIT_READY(connected);

  Future<Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  Future<Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers));

  {
    Call call;
    call.set_type(Call::SUBSCRIBE);

    Call::Subscribe* subscribe = call.mutable_subscribe();
    subscribe->mutable_framework_info()->CopyFrom(v1::DEFAULT_FRAMEWORK_INFO);

    mesos.send(call);
  }

  AWAIT_READY(subscribed);

  v1::FrameworkID frameworkId = subscribed->framework_id();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->offers().empty());

  v1::executor::Mesos* execMesos = nullptr;

  EXPECT_CALL(*executor, connected(_))
    .WillOnce(v1::executor::SendSubscribe(frameworkId, evolve(executorId)));

  EXPECT_CALL(*executor, subscribed(_, _))
    .WillOnce(SaveArg<0>(&execMesos));

  EXPECT_CALL(*executor, launch(_, _))
    .WillOnce(v1::executor::SendUpdateFromTask(
        frameworkId, evolve(executorId), v1::TASK_RUNNING));

  Future<Nothing> acknowledged;
  EXPECT_CALL(*executor, acknowledged(_, _))
    .WillOnce(FutureSatisfy(&acknowledged));

  Future<Event::Update> update;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(FutureArg<1>(&update));

  const v1::Offer& offer = offers->offers(0);

  v1::TaskInfo taskInfo =
    evolve(createTask(devolve(offer), "", executorId));

  {
    Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.set_type(Call::ACCEPT);

    Call::Accept* accept = call.mutable_accept();
    accept->add_offer_ids()->CopyFrom(offers->offers(0).id());

    v1::Offer::Operation* operation = accept->add_operations();
    operation->set_type(v1::Offer::Operation::LAUNCH);
    operation->mutable_launch()->add_task_infos()->CopyFrom(taskInfo);

    mesos.send(call);
  }

  AWAIT_READY(acknowledged);
  AWAIT_READY(update);

  EXPECT_EQ(v1::TASK_RUNNING, update->status().state());
  ASSERT_TRUE(update->status().has_executor_id());
  EXPECT_EQ(executorId, devolve(update->status().executor_id()));

  Future<Nothing> disconnected;
  EXPECT_CALL(*scheduler, disconnected(_))
    .WillOnce(FutureSatisfy(&disconnected));

  // Failover the master.
  master->reset();
  master = StartMaster();
  ASSERT_SOME(master);

  AWAIT_READY(disconnected);

  // Have the agent re-register with the master.
  detector.appoint(master.get()->pid);

  // Ensure re-registration is complete.
  Clock::pause();
  Clock::settle();

  EXPECT_CALL(*executor, acknowledged(_, _));

  Future<v1::executor::Call> updateCall =
    FUTURE_HTTP_CALL(v1::executor::Call(),
                     v1::executor::Call::UPDATE,
                     _,
                     contentType);

  // Send a terminal status update while the task is an orphan i.e., the
  // framework has not reconnected.
  {
    v1::TaskStatus status;
    status.mutable_task_id()->CopyFrom(taskInfo.task_id());
    status.mutable_executor_id()->CopyFrom(evolve(executorId));
    status.set_state(v1::TASK_FINISHED);
    status.set_source(v1::TaskStatus::SOURCE_EXECUTOR);
    status.set_uuid(UUID::random().toBytes());

    v1::executor::Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.mutable_executor_id()->CopyFrom(evolve(executorId));

    call.set_type(v1::executor::Call::UPDATE);

    call.mutable_update()->mutable_status()->CopyFrom(status);

    execMesos->send(call);
  }

  AWAIT_READY(updateCall);

  // Ensure that the update is processed by the agent.
  Clock::settle();

  Future<Nothing> recoverResources =
    FUTURE_DISPATCH(_, &MesosAllocatorProcess::recoverResources);

  // Advance the clock for the task status update manager to retry with the
  // latest state of the task.
  Clock::advance(slave::STATUS_UPDATE_RETRY_INTERVAL_MIN);
  Clock::settle();

  // Ensure that the resources are successfully recovered.
  AWAIT_READY(recoverResources);

  // Ensure that the state of the task is updated to `TASK_FINISHED`
  // on the master. We don't expect the task to be displayed as a
  // "completed task", because the terminal status update has not yet
  // been ack'ed by the scheduler.
  {
    v1::master::Call call;
    call.set_type(v1::master::Call::GET_TASKS);

    process::http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
    headers["Accept"] = stringify(contentType);

    Future<Response> response = process::http::post(
        master.get()->pid,
        "api/v1",
        headers,
        serialize(contentType, call),
        stringify(contentType));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

    v1::master::Response::GetTasks tasks = deserialize<v1::master::Response>(
        contentType, response->body)->get_tasks();

    ASSERT_TRUE(tasks.IsInitialized());
    ASSERT_EQ(1, tasks.tasks().size());
    EXPECT_EQ(TASK_FINISHED, tasks.tasks(0).state());
    EXPECT_TRUE(tasks.orphan_tasks().empty());
    EXPECT_TRUE(tasks.completed_tasks().empty());
  }

  EXPECT_CALL(*executor, shutdown(_))
    .Times(AtMost(1));

  EXPECT_CALL(*executor, disconnected(_))
    .Times(AtMost(1));
}


// This test checks that the "/state" endpoint displays the correct
// information when the master fails over and an agent running one of
// the framework's tasks re-registers before the framework does.
TEST_F(MasterTest, FailoverAgentReregisterFirst)
{
  master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  StandaloneMasterDetector slaveDetector(master.get()->pid);
  Try<Owned<cluster::Slave>> slave = StartSlave(&slaveDetector);
  ASSERT_SOME(slave);

  StandaloneMasterDetector schedDetector(master.get()->pid);
  MockScheduler sched;
  TestingMesosSchedulerDriver driver(
      &sched, &schedDetector, DEFAULT_FRAMEWORK_INFO);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(frameworkId);

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  TaskInfo task = createTask(offers.get()[0], "sleep 100");

  Future<TaskStatus> startingStatus;
  Future<TaskStatus> runningStatus;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&startingStatus))
    .WillOnce(FutureArg<1>(&runningStatus));

  Future<Nothing> statusUpdateAck1 = FUTURE_DISPATCH(
      slave.get()->pid, &Slave::_statusUpdateAcknowledgement);

  Future<Nothing> statusUpdateAck2 = FUTURE_DISPATCH(
      slave.get()->pid, &Slave::_statusUpdateAcknowledgement);

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(startingStatus);
  EXPECT_EQ(TASK_STARTING, startingStatus->state());
  EXPECT_EQ(task.task_id(), startingStatus->task_id());

  AWAIT_READY(statusUpdateAck1);

  AWAIT_READY(runningStatus);
  EXPECT_EQ(TASK_RUNNING, runningStatus->state());
  EXPECT_EQ(task.task_id(), runningStatus->task_id());

  AWAIT_READY(statusUpdateAck2);

  // Simulate master failover. We leave the scheduler without a master
  // so it does not attempt to re-register yet.
  EXPECT_CALL(sched, disconnected(&driver));

  schedDetector.appoint(None());
  slaveDetector.appoint(None());

  master->reset();
  master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Future<SlaveReregisteredMessage> slaveReregisteredMessage =
    FUTURE_PROTOBUF(SlaveReregisteredMessage(), _, _);

  slaveDetector.appoint(master.get()->pid);

  AWAIT_READY(slaveReregisteredMessage);

  // Check the master's "/state" endpoint. Because the slave has
  // re-registered, the master should know about the framework but
  // view it as disconnected and inactive.
  {
    Future<Response> response = process::http::get(
        master.get()->pid,
        "state",
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
    AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
    ASSERT_SOME(parse);

    JSON::Array frameworks = parse->values["frameworks"].as<JSON::Array>();

    ASSERT_EQ(1u, frameworks.values.size());

    JSON::Object framework = frameworks.values.front().as<JSON::Object>();

    EXPECT_EQ(
        frameworkId.get(),
        framework.values["id"].as<JSON::String>().value);

    EXPECT_FALSE(framework.values["active"].as<JSON::Boolean>().value);
    EXPECT_FALSE(framework.values["connected"].as<JSON::Boolean>().value);
    EXPECT_TRUE(framework.values["recovered"].as<JSON::Boolean>().value);
    EXPECT_EQ(0u, framework.values["registered_time"].as<JSON::Number>());
    EXPECT_EQ(0u, framework.values["unregistered_time"].as<JSON::Number>());
    EXPECT_EQ(0u, framework.values.count("reregistered_time"));

    JSON::Array unregisteredFrameworks =
      parse->values["unregistered_frameworks"].as<JSON::Array>();

    EXPECT_TRUE(unregisteredFrameworks.values.empty());

    JSON::Array completedFrameworks =
      parse->values["completed_frameworks"].as<JSON::Array>();

    EXPECT_TRUE(completedFrameworks.values.empty());
  }

  // Cause the scheduler to re-register. We pause the clock to ensure
  // the re-registration time is predictable. We get a "registered"
  // callback in the scheduler driver because of MESOS-786.
  Future<Nothing> registered;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureSatisfy(&registered));

  Clock::pause();
  process::Time reregisterTime = Clock::now();

  schedDetector.appoint(master.get()->pid);
  AWAIT_READY(registered);

  Clock::resume();

  // Check the master's "/state" endpoint. The framework should now be
  // listed as connected and active.
  {
    Future<Response> response = process::http::get(
        master.get()->pid,
        "state",
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
    AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
    ASSERT_SOME(parse);

    JSON::Array frameworks = parse->values["frameworks"].as<JSON::Array>();

    ASSERT_EQ(1u, frameworks.values.size());

    JSON::Object framework = frameworks.values.front().as<JSON::Object>();

    EXPECT_EQ(
        frameworkId.get(),
        framework.values["id"].as<JSON::String>().value);

    EXPECT_TRUE(framework.values["active"].as<JSON::Boolean>().value);
    EXPECT_TRUE(framework.values["connected"].as<JSON::Boolean>().value);
    EXPECT_FALSE(framework.values["recovered"].as<JSON::Boolean>().value);
    EXPECT_EQ(0u, framework.values["unregistered_time"].as<JSON::Number>());

    // Even with a paused clock, the value of `registered_time` and
    // `reregistered_time` from the state endpoint can differ slightly
    // from the actual start time since the value went through a
    // number of conversions (`double` to `string` to `JSON::Value`).
    // Since `Clock::now` is a floating point value, the actual
    // maximal possible difference between the real and observed value
    // depends on both the mantissa and the exponent of the compared
    // values; for simplicity we compare with an epsilon of `1` which
    // allows for e.g., changes in the integer part of values close to
    // an integer value.
    EXPECT_NEAR(
        reregisterTime.secs(),
        framework.values["registered_time"].as<JSON::Number>().as<double>(),
        1);

    // The state endpoint does not return "reregistered_time" if it is
    // the same as "registered_time".
    EXPECT_EQ(0, framework.values.count("reregistered_time"));

    JSON::Array unregisteredFrameworks =
      parse->values["unregistered_frameworks"].as<JSON::Array>();

    EXPECT_TRUE(unregisteredFrameworks.values.empty());

    JSON::Array completedFrameworks =
      parse->values["completed_frameworks"].as<JSON::Array>();

    EXPECT_TRUE(completedFrameworks.values.empty());
  }

  driver.stop();
  driver.join();
}


// In this test, an agent restarts, responds to pings, but does not
// re-register with the master; the master should mark the agent
// unreachable after waiting for `agent_reregister_timeout`. In
// practice, this typically happens because agent recovery hangs; to
// simplify the test case, we instead drop the agent -> master
// re-registration message.
TEST_F(MasterTest, AgentRestartNoReregister)
{
  // We disable agent authentication to simplify the messages we need
  // to drop to prevent agent re-registration below.
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.authenticate_agents = false;

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  slave::Flags agentFlags = CreateSlaveFlags();
  agentFlags.credential = None();

  mesos::internal::slave::Fetcher fetcher(agentFlags);

  Try<MesosContainerizer*> _containerizer =
    MesosContainerizer::create(agentFlags, true, &fetcher);

  ASSERT_SOME(_containerizer);
  Owned<MesosContainerizer> containerizer(_containerizer.get());

  StandaloneMasterDetector detector(master.get()->pid);

  // We use the same UPID when we restart the agent below, so that the
  // agent continues to receive pings from the master before it
  // successfully re-registers.
  const string agentPid = "agent";

  Try<Owned<cluster::Slave>> slave =
    StartSlave(&detector, containerizer.get(), agentPid, agentFlags);
  ASSERT_SOME(slave);

  // Start a partition-aware scheduler with checkpointing.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_checkpoint(true);
  frameworkInfo.add_capabilities()->set_type(
      FrameworkInfo::Capability::PARTITION_AWARE);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  Offer offer = offers.get()[0];

  TaskInfo task = createTask(offer, "sleep 100");

  Future<TaskStatus> startingStatus;
  Future<TaskStatus> runningStatus;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&startingStatus))
    .WillOnce(FutureArg<1>(&runningStatus));

  Future<Nothing> statusUpdateAck1 = FUTURE_DISPATCH(
    slave.get()->pid, &Slave::_statusUpdateAcknowledgement);

  Future<Nothing> statusUpdateAck2 = FUTURE_DISPATCH(
    slave.get()->pid, &Slave::_statusUpdateAcknowledgement);

  driver.launchTasks(offer.id(), {task});

  AWAIT_READY(startingStatus);
  EXPECT_EQ(TASK_STARTING, startingStatus->state());
  EXPECT_EQ(task.task_id(), startingStatus->task_id());

  AWAIT_READY(statusUpdateAck1);

  AWAIT_READY(runningStatus);
  EXPECT_EQ(TASK_RUNNING, runningStatus->state());
  EXPECT_EQ(task.task_id(), runningStatus->task_id());

  const SlaveID slaveId = runningStatus->slave_id();

  AWAIT_READY(statusUpdateAck2);

  Clock::pause();

  // Terminate the agent abruptly. This causes the master -> agent
  // socket to break on the master side.
  slave.get()->terminate();

  Future<ReregisterExecutorMessage> reregisterExecutorMessage =
    FUTURE_PROTOBUF(ReregisterExecutorMessage(), _, _);

  Future<ReregisterSlaveMessage> reregisterSlave1 =
    DROP_PROTOBUF(ReregisterSlaveMessage(), _, _);

  Future<PingSlaveMessage> ping = FUTURE_PROTOBUF(PingSlaveMessage(), _, _);
  Future<PongSlaveMessage> pong = FUTURE_PROTOBUF(PongSlaveMessage(), _, _);

  _containerizer = MesosContainerizer::create(agentFlags, true, &fetcher);
  ASSERT_SOME(_containerizer);
  containerizer.reset(_containerizer.get());

  // Restart the agent using the same UPID.
  slave = StartSlave(&detector, containerizer.get(), agentPid, agentFlags);
  ASSERT_SOME(slave);

  // Wait for the executor to re-register.
  AWAIT_READY(reregisterExecutorMessage);

  // The agent waits for the executor reregister timeout to expire,
  // even if all executors have re-reregistered.
  Clock::advance(agentFlags.executor_reregistration_timeout);
  Clock::settle();

  // Agent will try to re-register after completing recovery; prevent
  // this from succeeding by dropping the re-reregistration message.
  Clock::advance(agentFlags.registration_backoff_factor);
  AWAIT_READY(reregisterSlave1);

  // Drop subsequent re-registration attempts, until we allow
  // re-registration to succeed below.
  DROP_PROTOBUFS(ReregisterSlaveMessage(), _, _);

  // The agent should receive pings from the master and reply to them.
  Clock::advance(masterFlags.agent_ping_timeout);

  AWAIT_READY(ping);
  AWAIT_READY(pong);

  EXPECT_FALSE(ping->connected());

  // If the agent hasn't recovered within `agent_reregister_timeout`,
  // the master should mark it unreachable.
  Future<Nothing> slaveLost;
  EXPECT_CALL(sched, slaveLost(&driver, _))
    .WillOnce(FutureSatisfy(&slaveLost));

  Future<TaskStatus> unreachableStatus;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&unreachableStatus));

  Duration elapsedTime =
    masterFlags.agent_ping_timeout + agentFlags.executor_reregistration_timeout;

  Duration remainingReregisterTime =
    masterFlags.agent_reregister_timeout - elapsedTime;

  Clock::advance(remainingReregisterTime);

  TimeInfo unreachableTime = protobuf::getCurrentTime();

  AWAIT_READY(slaveLost);

  AWAIT_READY(unreachableStatus);
  EXPECT_EQ(TASK_UNREACHABLE, unreachableStatus->state());
  EXPECT_EQ(TaskStatus::REASON_SLAVE_REMOVED, unreachableStatus->reason());
  EXPECT_EQ(task.task_id(), unreachableStatus->task_id());
  EXPECT_EQ(slaveId, unreachableStatus->slave_id());
  EXPECT_EQ(unreachableTime, unreachableStatus->unreachable_time());

  // Allow agent re-registration to succeed.
  Future<ReregisterSlaveMessage> reregisterSlave2 =
    FUTURE_PROTOBUF(ReregisterSlaveMessage(), _, _);

  Future<SlaveReregisteredMessage> slaveReregistered =
    FUTURE_PROTOBUF(SlaveReregisteredMessage(), _, _);

  detector.appoint(master.get()->pid);

  Clock::advance(agentFlags.registration_backoff_factor);

  AWAIT_READY(reregisterSlave2);
  AWAIT_READY(slaveReregistered);

  Clock::resume();

  TaskStatus status;
  status.mutable_task_id()->CopyFrom(task.task_id());
  status.mutable_slave_id()->CopyFrom(slaveId);
  status.set_state(TASK_STAGING); // Dummy value.

  Future<TaskStatus> reconcileUpdate;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&reconcileUpdate));

  driver.reconcileTasks({status});

  AWAIT_READY(reconcileUpdate);
  EXPECT_EQ(TASK_RUNNING, reconcileUpdate->state());
  EXPECT_EQ(TaskStatus::REASON_RECONCILIATION, reconcileUpdate->reason());

  // Check metrics.
  JSON::Object stats = Metrics();
  EXPECT_EQ(0, stats.values["master/recovery_slave_removals"]);
  EXPECT_EQ(1, stats.values["master/slave_removals"]);
  EXPECT_EQ(1, stats.values["master/slave_removals/reason_unhealthy"]);
  EXPECT_EQ(0, stats.values["master/slave_removals/reason_unregistered"]);
  EXPECT_EQ(1, stats.values["master/slave_unreachable_completed"]);
  EXPECT_EQ(1, stats.values["master/slave_unreachable_scheduled"]);

  driver.stop();
  driver.join();
}


// When removing agents that haven't re-registered after a socket
// error (see notes in `AgentRestartNoReregister`) above, this test
// checks that the master respects the agent removal rate limit.
TEST_F(MasterTest, AgentRestartNoReregisterRateLimit)
{
  // Start a master.
  auto slaveRemovalLimiter = std::make_shared<MockRateLimiter>();
  master::Flags masterFlags = CreateMasterFlags();

  Try<Owned<cluster::Master>> master =
    StartMaster(slaveRemovalLimiter, masterFlags);
  ASSERT_SOME(master);

  slave::Flags agentFlags = CreateSlaveFlags();
  mesos::internal::slave::Fetcher fetcher(agentFlags);

  Try<MesosContainerizer*> _containerizer =
    MesosContainerizer::create(agentFlags, true, &fetcher);

  ASSERT_SOME(_containerizer);
  Owned<MesosContainerizer> containerizer(_containerizer.get());

  StandaloneMasterDetector detector(master.get()->pid);

  // We use the same UPID when we restart the agent below, so that the
  // agent continues to receive pings from the master before it
  // successfully re-registers.
  const string agentPid = "agent";

  Try<Owned<cluster::Slave>> slave =
    StartSlave(&detector, containerizer.get(), agentPid, agentFlags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<Nothing> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureSatisfy(&offers));

  driver.start();

  AWAIT_READY(offers);

  EXPECT_CALL(sched, offerRescinded(&driver, _));

  Clock::pause();

  // Terminate the agent abruptly. This causes the master -> agent
  // socket to break on the master side.
  slave.get()->terminate();

  Future<ReregisterSlaveMessage> reregisterSlave =
    DROP_PROTOBUF(ReregisterSlaveMessage(), _, _);

  Future<PingSlaveMessage> ping = FUTURE_PROTOBUF(PingSlaveMessage(), _, _);
  Future<PongSlaveMessage> pong = FUTURE_PROTOBUF(PongSlaveMessage(), _, _);

  _containerizer = MesosContainerizer::create(agentFlags, true, &fetcher);
  ASSERT_SOME(_containerizer);
  containerizer.reset(_containerizer.get());

  // Restart the agent using the same UPID.
  slave = StartSlave(&detector, containerizer.get(), agentPid, agentFlags);
  ASSERT_SOME(slave);

  // Agent will try to re-register after completing recovery; prevent
  // this from succeeding by dropping the re-reregistration message.
  Clock::advance(agentFlags.registration_backoff_factor);
  AWAIT_READY(reregisterSlave);

  // Drop subsequent re-registration attempts.
  DROP_PROTOBUFS(ReregisterSlaveMessage(), _, _);

  // The agent should receive pings from the master and reply to them.
  Clock::advance(masterFlags.agent_ping_timeout);

  AWAIT_READY(ping);
  AWAIT_READY(pong);

  EXPECT_FALSE(ping->connected());

  // Return a pending future from the rate limiter.
  Future<Nothing> acquire;
  Promise<Nothing> promise;
  EXPECT_CALL(*slaveRemovalLimiter, acquire())
    .WillOnce(DoAll(FutureSatisfy(&acquire),
                    Return(promise.future())));

  // If the agent hasn't recovered within `agent_reregister_timeout`,
  // the master should start to mark it unreachable, once permitted by
  // the rate limiter.
  Future<Nothing> slaveLost;
  EXPECT_CALL(sched, slaveLost(&driver, _))
    .WillOnce(FutureSatisfy(&slaveLost));

  Duration remainingReregisterTime =
    masterFlags.agent_reregister_timeout - masterFlags.agent_ping_timeout;

  Clock::advance(remainingReregisterTime);

  // The master should attempt to acquire a permit.
  AWAIT_READY(acquire);

  // The slave should not be removed before the permit is satisfied;
  // that means the scheduler shouldn't receive `slaveLost` yet.
  Clock::settle();
  ASSERT_TRUE(slaveLost.isPending());

  // Once the permit is satisfied, the `slaveLost` scheduler callback
  // should be invoked.
  promise.set(Nothing());
  AWAIT_READY(slaveLost);

  driver.stop();
  driver.join();
}


// This test ensures that a multi-role framework can receive offers
// for different roles it subscribes with. We start two slaves and
// launch one multi-role framework with two roles. The framework should
// receive two offers, one for each slave, allocated to different roles.
TEST_F(MasterTest, MultiRoleFrameworkReceivesOffers)
{
  Clock::pause();

  master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  // Start first agent.
  Try<Owned<cluster::Slave>> slave1 = StartSlave(detector.get());
  ASSERT_SOME(slave1);

  FrameworkInfo framework = DEFAULT_FRAMEWORK_INFO;
  framework.set_roles(0, "role1");
  framework.add_roles("role2");

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, framework, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<Nothing> registered;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureSatisfy(&registered));

  driver.start();

  Clock::settle();

  AWAIT_READY(registered);

  Future<vector<Offer>> offers1;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers1));

  Clock::advance(masterFlags.allocation_interval);
  Clock::settle();

  AWAIT_READY(offers1);
  ASSERT_FALSE(offers1->empty());
  ASSERT_TRUE(offers1.get()[0].has_allocation_info());

  // Start second agent.
  Try<Owned<cluster::Slave>> slave2 = StartSlave(detector.get());
  ASSERT_SOME(slave2);

  Future<vector<Offer>> offers2;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers2));

  Clock::advance(masterFlags.allocation_interval);
  Clock::settle();

  AWAIT_READY(offers2);
  ASSERT_FALSE(offers2->empty());
  ASSERT_TRUE(offers2.get()[0].has_allocation_info());

  // We cannot deterministically expect roles for each offer, however we
  // could assert that 1st and 2nd offers should have different roles.
  ASSERT_NE(
      offers1.get()[0].allocation_info().role(),
      offers2.get()[0].allocation_info().role());

  driver.stop();
  driver.join();
}


TEST_F(MasterTest, TaskWithTinyResources)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  Offer offer = offers.get()[0];

  TaskInfo task = createTask(
      offer.slave_id(),
      Resources::parse("cpus:0.00001;mem:1").get(),
      SLEEP_COMMAND(1000));

  Future<TaskStatus> startingStatus;
  Future<TaskStatus> runningStatus;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&startingStatus))
    .WillOnce(FutureArg<1>(&runningStatus));

  driver.launchTasks(offer.id(), {task});

  AWAIT_READY(startingStatus);
  EXPECT_EQ(TASK_STARTING, startingStatus->state());
  EXPECT_EQ(task.task_id(), startingStatus->task_id());

  AWAIT_READY(runningStatus);
  EXPECT_EQ(TASK_RUNNING, runningStatus->state());
  EXPECT_EQ(task.task_id(), runningStatus->task_id());

  driver.stop();
  driver.join();
}


// This test ensures that when a partitioned agent comes back with tasks that
// are allocated to a role that a framework is no longer subscribed to,
// the framework is re-tracked under the role, but still does not receive
// any offers with resources allocated to that role.
TEST_F(MasterTest, MultiRoleSchedulerUnsubscribeFromRole)
{
  // Manipulate the clock manually.
  Clock::pause();

  master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Allow the master to PING the agent, but drop all PONG messages
  // from the agent. Note that we don't match on the master / agent
  // PIDs because it's actually the `SlaveObserver` process that sends
  // the pings.
  Future<Message> ping = FUTURE_MESSAGE(
      Eq(PingSlaveMessage().GetTypeName()), _, _);

  DROP_PROTOBUFS(PongSlaveMessage(), _, _);

  StandaloneMasterDetector detector(master.get()->pid);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  slave::Flags agentFlags = CreateSlaveFlags();
  agentFlags.resources = "cpus:2;mem:2048";

  Try<Owned<cluster::Slave>> agent = StartSlave(&detector, agentFlags);
  ASSERT_SOME(agent);

  Clock::advance(agentFlags.registration_backoff_factor);

  AWAIT_READY(slaveRegisteredMessage);
  SlaveID agentId = slaveRegisteredMessage->slave_id();

  // Start a scheduler. The scheduler has the PARTITION_AWARE
  // capability, so we expect its tasks to continue running when the
  // partitioned agent reregisters.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_roles(0, "foo");
  frameworkInfo.add_capabilities()->set_type(
      FrameworkInfo::Capability::PARTITION_AWARE);

  MockScheduler sched1;
  MesosSchedulerDriver driver1(
      &sched1, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched1, registered(&driver1, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched1, resourceOffers(&driver1, _))
    .WillOnce(FutureArg<1>(&offers));

  driver1.start();

  Clock::advance(masterFlags.allocation_interval);

  AWAIT_READY(frameworkId);
  AWAIT_READY(offers);
  ASSERT_EQ(1u, offers->size());

  Resources resources = Resources::parse("cpus:1;mem:512").get();

  Offer offer = offers.get()[0];

  TaskInfo task = createTask(offer.slave_id(), resources, "sleep 60");

  Future<TaskStatus> startingStatus;
  Future<TaskStatus> runningStatus;
  EXPECT_CALL(sched1, statusUpdate(&driver1, _))
    .WillOnce(FutureArg<1>(&startingStatus))
    .WillOnce(FutureArg<1>(&runningStatus));

  driver1.launchTasks(offer.id(), {task});

  AWAIT_READY(startingStatus);
  EXPECT_EQ(TASK_STARTING, startingStatus->state());
  EXPECT_EQ(task.task_id(), startingStatus->task_id());

  AWAIT_READY(runningStatus);
  EXPECT_EQ(TASK_RUNNING, runningStatus->state());
  EXPECT_EQ(task.task_id(), runningStatus->task_id());

  // Remove the role from the framework.

  frameworkInfo.mutable_id()->CopyFrom(frameworkId.get());
  frameworkInfo.clear_roles();

  MockScheduler sched2;
  MesosSchedulerDriver driver2(
      &sched2, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<Nothing> registered2;
  EXPECT_CALL(sched2, registered(&driver2, frameworkId.get(), _))
    .WillOnce(FutureSatisfy(&registered2));

  Future<UpdateFrameworkMessage> updateFrameworkMessage =
    FUTURE_PROTOBUF(UpdateFrameworkMessage(), _, _);

  // Scheduler1 should get an error due to failover.
  EXPECT_CALL(sched1, error(&driver1, "Framework failed over"));

  // Expect that there will be no resource offers made to the scheduler.
  EXPECT_CALL(sched2, resourceOffers(&driver2, _)).Times(0);

  driver2.start();

  Clock::advance(masterFlags.allocation_interval);

  AWAIT_READY(registered2);

  // Wait for the agent to get the updated framework info.
  AWAIT_READY(updateFrameworkMessage);

  driver1.stop();
  driver1.join();

  // Now, induce a partition of the slave by having the master
  // timeout the slave.
  Future<TaskStatus> unreachableStatus;
  EXPECT_CALL(sched2, statusUpdate(&driver2, _))
    .WillOnce(FutureArg<1>(&unreachableStatus));

  // We expect to get a `slaveLost` callback, even though this
  // scheduler is partition-aware.
  Future<Nothing> agentLost;
  EXPECT_CALL(sched2, slaveLost(&driver2, _))
    .WillOnce(FutureSatisfy(&agentLost));

  size_t pings = 0;
  while (true) {
    AWAIT_READY(ping);
    pings++;
    if (pings == masterFlags.max_agent_ping_timeouts) {
      break;
    }
    ping = FUTURE_MESSAGE(Eq(PingSlaveMessage().GetTypeName()), _, _);
    Clock::advance(masterFlags.agent_ping_timeout);
  }

  Clock::advance(masterFlags.agent_ping_timeout);

  AWAIT_READY(unreachableStatus);
  EXPECT_EQ(TASK_UNREACHABLE, unreachableStatus->state());
  EXPECT_EQ(TaskStatus::REASON_SLAVE_REMOVED, unreachableStatus->reason());
  EXPECT_EQ(task.task_id(), unreachableStatus->task_id());
  EXPECT_EQ(agentId, unreachableStatus->slave_id());

  AWAIT_READY(agentLost);

  // Check that the framework is not tracked under the role.
  {
    Future<Response> response = process::http::get(
        master.get()->pid,
        "roles",
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
    AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
    ASSERT_SOME(parse);

    const JSON::Object& result = parse.get();

    JSON::Object expected = {
      {"roles", JSON::Array{}}
    };

    EXPECT_EQ(expected, result);
  }

  // We now complete the partition on the agent side as well. We simulate
  // a master loss event, which would normally happen during a network
  // partition. The slave should then reregister with the master.
  // The master will then re-track the framework under the role, but the
  // framework should not receive any resources allocated to the role.
  detector.appoint(None());

  Future<SlaveReregisteredMessage> agentReregistered = FUTURE_PROTOBUF(
      SlaveReregisteredMessage(), master.get()->pid, agent.get()->pid);

  detector.appoint(master.get()->pid);

  Clock::advance(agentFlags.registration_backoff_factor);

  AWAIT_READY(agentReregistered);

  // Check that the framework is re-tracked under the role by the master.
  {
    Future<Response> response = process::http::get(
        master.get()->pid,
        "roles",
        None(),
        createBasicAuthHeaders(DEFAULT_CREDENTIAL));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
    AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

    Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
    ASSERT_SOME(parse);

    JSON::Value result = parse.get();

    JSON::Object expected = {
      {
        "roles",
        JSON::Array {
          JSON::Object {
            { "name", "foo" },
            { "frameworks", JSON::Array { frameworkId.get().value() } }
          }
        }
      }
    };

    EXPECT_TRUE(result.contains(expected));
  }

  // Ensure allocations to be made.
  Clock::advance(masterFlags.allocation_interval);

  Clock::settle();
  Clock::resume();

  driver2.stop();
  driver2.join();
}


// This test checks that if the agent and master are configured with
// domains that specify the same region (but different zones), the
// agent is allowed to register and its resources are offered to
// frameworks as usual.
TEST_F(MasterTest, AgentDomainSameRegion)
{
  Clock::pause();

  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.domain = createDomainInfo("region-abc", "zone-123");

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.domain = createDomainInfo("region-abc", "zone-456");

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  Clock::advance(slaveFlags.registration_backoff_factor);
  AWAIT_READY(slaveRegisteredMessage);

  const SlaveID& slaveId = slaveRegisteredMessage->slave_id();

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<MasterInfo> masterInfo;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<2>(&masterInfo));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  AWAIT_READY(masterInfo);
  EXPECT_EQ(masterFlags.domain, masterInfo->domain());

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  Offer offer = offers->front();
  EXPECT_EQ(slaveId, offer.slave_id());
  EXPECT_EQ(slaveFlags.domain.get(), offer.domain());

  driver.stop();
  driver.join();
}


// This test checks that if the agent and master are configured with
// domains that specify different regions, the agent is allowed to
// register but its resources are only offered to region-aware
// frameworks. We also check that tasks can be launched in remote
// regions.
TEST_F(MasterTest, AgentDomainDifferentRegion)
{
  Clock::pause();

  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.domain = createDomainInfo("region-abc", "zone-123");

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.domain = createDomainInfo("region-xyz", "zone-123");

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  Clock::advance(slaveFlags.registration_backoff_factor);
  AWAIT_READY(slaveRegisteredMessage);

  const SlaveID& slaveId = slaveRegisteredMessage->slave_id();

  // Launch a non-region-aware scheduler. It should NOT receive any
  // resource offers for `slave`.
  {
    MockScheduler sched;
    MesosSchedulerDriver driver(
        &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

    Future<Nothing> registered;
    EXPECT_CALL(sched, registered(&driver, _, _))
      .WillOnce(FutureSatisfy(&registered));

    // We do not expect to get offered any resources.
    Future<vector<Offer>> offers;
    EXPECT_CALL(sched, resourceOffers(&driver, _))
      .Times(0);

    driver.start();

    AWAIT_READY(registered);

    // Trigger a batch allocation, for good measure.
    Clock::advance(masterFlags.allocation_interval);
    Clock::settle();

    driver.stop();
    driver.join();
  }

  // Launch a region-aware scheduler. It should receive an offer for `slave`.
  {
    FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
    frameworkInfo.add_capabilities()->set_type(
        FrameworkInfo::Capability::REGION_AWARE);

    MockScheduler sched;
    MesosSchedulerDriver driver(
        &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

    EXPECT_CALL(sched, registered(&driver, _, _));

    Future<vector<Offer>> offers;
    EXPECT_CALL(sched, resourceOffers(&driver, _))
      .WillOnce(FutureArg<1>(&offers));

    driver.start();

    AWAIT_READY(offers);
    ASSERT_FALSE(offers->empty());

    Offer offer = offers->front();
    EXPECT_EQ(slaveId, offer.slave_id());
    EXPECT_EQ(slaveFlags.domain.get(), offer.domain());

    // Check that we can launch a task in a remote region.
    TaskInfo task = createTask(offer, "sleep 60");

    Future<TaskStatus> startingStatus;
    Future<TaskStatus> runningStatus;
    EXPECT_CALL(sched, statusUpdate(&driver, _))
      .WillOnce(FutureArg<1>(&startingStatus))
      .WillOnce(FutureArg<1>(&runningStatus));

    driver.launchTasks(offer.id(), {task});

    AWAIT_READY(startingStatus);
    EXPECT_EQ(TASK_STARTING, startingStatus->state());
    EXPECT_EQ(task.task_id(), startingStatus->task_id());

    AWAIT_READY(runningStatus);
    EXPECT_EQ(TASK_RUNNING, runningStatus->state());
    EXPECT_EQ(task.task_id(), runningStatus->task_id());

    driver.stop();
    driver.join();
  }

  // Resume the clock so that executor/task cleanup happens correctly.
  //
  // TODO(neilc): Replace this with more fine-grained clock advancement.
  Clock::resume();
}


// This test checks that if the master is configured with a domain but
// the agent is not, the agent is allowed to register and its
// resources are offered to frameworks as usual.
TEST_F(MasterTest, AgentDomainUnset)
{
  Clock::pause();

  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.domain = createDomainInfo("region-abc", "zone-123");

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  slave::Flags slaveFlags = CreateSlaveFlags();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  Clock::advance(slaveFlags.registration_backoff_factor);
  AWAIT_READY(slaveRegisteredMessage);

  const SlaveID& slaveId = slaveRegisteredMessage->slave_id();

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  Offer offer = offers->front();
  EXPECT_EQ(slaveId, offer.slave_id());
  EXPECT_FALSE(offer.has_domain());

  driver.stop();
  driver.join();
}


// This test checks that if the agent is configured with a domain but
// the master is not, the agent is not allowed to register.
TEST_F(MasterTest, AgentDomainMismatch)
{
  Clock::pause();

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.domain = createDomainInfo("region-abc", "zone-456");

  // Agent should attempt to register.
  Future<RegisterSlaveMessage> registerSlaveMessage =
    FUTURE_PROTOBUF(RegisterSlaveMessage(), _, _);

  // If the agent is allowed to register, the master will update the
  // registry. The agent should not be allowed to register, so we
  // expect that no registrar operations will be observed.
  EXPECT_CALL(*master.get()->registrar, apply(_))
    .Times(0);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  Clock::advance(slaveFlags.registration_backoff_factor);
  AWAIT_READY(registerSlaveMessage);

  Clock::settle();
}


// This test checks that if the agent is configured with a domain but
// the master is not, the agent is not allowed to re-register. This
// might happen if the leading master is configured with a domain but
// one of the standby masters is not, and then the leader fails over.
TEST_F(MasterTest, AgentDomainMismatchOnReregister)
{
  Clock::pause();

  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.domain = createDomainInfo("region-abc", "zone-123");

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.domain = createDomainInfo("region-abc", "zone-456");

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  StandaloneMasterDetector detector(master.get()->pid);
  Try<Owned<cluster::Slave>> slave = StartSlave(&detector, slaveFlags);
  ASSERT_SOME(slave);

  Clock::advance(slaveFlags.registration_backoff_factor);
  AWAIT_READY(slaveRegisteredMessage);

  // Simulate master failover and start a new master with no domain
  // configured.
  master->reset();

  masterFlags.domain = None();

  master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Future<ReregisterSlaveMessage> reregisterSlaveMessage =
    FUTURE_PROTOBUF(ReregisterSlaveMessage(), _, _);

  // If the agent is allowed to re-register, the master will update
  // the registry. The agent should not be allowed to register, so we
  // expect that no registrar operations will be observed.
  EXPECT_CALL(*master.get()->registrar, apply(_))
    .Times(0);

  // Simulate a new master detected event.
  detector.appoint(master.get()->pid);

  Clock::advance(slaveFlags.registration_backoff_factor);
  AWAIT_READY(reregisterSlaveMessage);

  Clock::settle();
}


// Check that the master does not allow old Mesos agents to register.
// We do this by intercepting the agent's `RegisterSlaveMessage` and
// then re-sending it with a tweaked version number.
TEST_F(MasterTest, IgnoreOldAgentRegistration)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Future<RegisterSlaveMessage> registerSlaveMessage =
    DROP_PROTOBUF(RegisterSlaveMessage(), _, _);

  Clock::pause();

  slave::Flags slaveFlags = CreateSlaveFlags();
  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  Clock::advance(slaveFlags.authentication_backoff_factor);
  Clock::advance(slaveFlags.registration_backoff_factor);

  AWAIT_READY(registerSlaveMessage);

  RegisterSlaveMessage message = registerSlaveMessage.get();
  message.set_version("0.28.1-rc1");

  // The master should ignore the agent's registration attempt. Hence,
  // we do not expect the master to try to update the registry.
  EXPECT_CALL(*master.get()->registrar, apply(_))
    .Times(0);

  process::post(slave.get()->pid, master.get()->pid, message);

  // Settle the clock to retire in-flight messages.
  Clock::settle();
}


// Check that the master does not allow old Mesos agents to re-register.
// We do this by intercepting the agent's `ReregisterSlaveMessage` and
// then re-sending it with a tweaked version number.
TEST_F(MasterTest, IgnoreOldAgentReregistration)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  Clock::pause();

  StandaloneMasterDetector detector(master.get()->pid);
  slave::Flags slaveFlags = CreateSlaveFlags();
  Try<Owned<cluster::Slave>> slave = StartSlave(&detector, slaveFlags);
  ASSERT_SOME(slave);

  Clock::settle();
  Clock::advance(slaveFlags.registration_backoff_factor);

  AWAIT_READY(slaveRegisteredMessage);

  // Intercept the agent's `ReregisterSlaveMessage`.
  Future<ReregisterSlaveMessage> reregisterSlaveMessage =
    DROP_PROTOBUF(ReregisterSlaveMessage(), _, _);

  // Simulate a new master detected event on the slave,
  // so that the slave will attempt to re-register.
  detector.appoint(master.get()->pid);

  Clock::settle();
  Clock::advance(slaveFlags.registration_backoff_factor);

  AWAIT_READY(reregisterSlaveMessage);

  ReregisterSlaveMessage message = reregisterSlaveMessage.get();
  message.set_version("0.28.1-rc1");

  // The master should ignore the agent's re-registration attempt, so
  // we do not expect the master to try to update the registry.
  EXPECT_CALL(*master.get()->registrar, apply(_))
    .Times(0);

  process::post(slave.get()->pid, master.get()->pid, message);

  // Settle the clock to retire in-flight messages.
  Clock::settle();
}


// This test checks that the master correctly garbage collects
// information about gone agents from the registry using the
// count-based GC criterion.
//
// TODO(andschwa): Enable this when MESOS-7604 is fixed.
TEST_F_TEMP_DISABLED_ON_WINDOWS(MasterTest, RegistryGcByCount)
{
  // Configure GC to only keep the most recent gone agent in the gone list.
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.registry_max_agent_count = 1;

  Try<Owned<cluster::Master>> master = this->StartMaster(masterFlags);
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), master.get()->pid, _);

  slave::Flags slaveFlags = CreateSlaveFlags();

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  // Ensure that the agent is registered successfully with the master
  // before marking it as gone.
  AWAIT_READY(slaveRegisteredMessage);

  ContentType contentType = ContentType::PROTOBUF;

  const SlaveID& slaveId = slaveRegisteredMessage->slave_id();

  {
    v1::master::Call v1Call;
    v1Call.set_type(v1::master::Call::MARK_AGENT_GONE);

    v1::master::Call::MarkAgentGone* markAgentGone =
      v1Call.mutable_mark_agent_gone();

    markAgentGone->mutable_agent_id()->CopyFrom(evolve(slaveId));

    Future<process::http::Response> response = process::http::post(
        master.get()->pid,
        "api/v1",
        createBasicAuthHeaders(DEFAULT_CREDENTIAL),
        serialize(contentType, v1Call),
        stringify(contentType));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(process::http::OK().status, response);
  }

  Future<SlaveRegisteredMessage> slaveRegisteredMessage2 =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), master.get()->pid, _);

  slave::Flags slaveFlags2 = CreateSlaveFlags();

  Try<Owned<cluster::Slave>> slave2 = StartSlave(detector.get(), slaveFlags2);
  ASSERT_SOME(slave2);

  AWAIT_READY(slaveRegisteredMessage2);

  const SlaveID& slaveId2 = slaveRegisteredMessage2->slave_id();

  {
    v1::master::Call v1Call;
    v1Call.set_type(v1::master::Call::MARK_AGENT_GONE);

    v1::master::Call::MarkAgentGone* markAgentGone =
      v1Call.mutable_mark_agent_gone();

    markAgentGone->mutable_agent_id()->CopyFrom(evolve(slaveId2));

    Future<process::http::Response> response = process::http::post(
        master.get()->pid,
        "api/v1",
        createBasicAuthHeaders(DEFAULT_CREDENTIAL),
        serialize(contentType, v1Call),
        stringify(contentType));

    AWAIT_EXPECT_RESPONSE_STATUS_EQ(process::http::OK().status, response);
  }

  // Advance the clock to cause GC to be performed.
  Clock::pause();
  Clock::advance(masterFlags.registry_gc_interval);
  Clock::settle();

  // Start a framework and do explicit reconciliation for a random task ID
  // on `slave1` and `slave2`. Since, `slave1` has been GC'ed from the list
  // of gone agents, a 'TASK_UNKNOWN' update should be received for it.

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.add_capabilities()->set_type(
      FrameworkInfo::Capability::PARTITION_AWARE);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<Nothing> registered;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureSatisfy(&registered));

  driver.start();

  AWAIT_READY(registered);

  TaskStatus status1;
  status1.mutable_task_id()->set_value(UUID::random().toString());
  status1.mutable_slave_id()->CopyFrom(slaveId);
  status1.set_state(TASK_STAGING); // Dummy value.

  TaskStatus status2;
  status2.mutable_task_id()->set_value(UUID::random().toString());
  status2.mutable_slave_id()->CopyFrom(slaveId2);
  status2.set_state(TASK_STAGING); // Dummy value.

  Future<TaskStatus> reconcileUpdate1;
  Future<TaskStatus> reconcileUpdate2;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&reconcileUpdate1))
    .WillOnce(FutureArg<1>(&reconcileUpdate2));

  driver.reconcileTasks({status1, status2});

  AWAIT_READY(reconcileUpdate1);
  AWAIT_READY(reconcileUpdate2);

  ASSERT_EQ(TASK_UNKNOWN, reconcileUpdate1->state());
  ASSERT_EQ(TASK_GONE_BY_OPERATOR, reconcileUpdate2->state());
}


class MasterTestPrePostReservationRefinement
  : public MasterTest,
    public WithParamInterface<bool> {
public:
  virtual master::Flags CreateMasterFlags()
  {
    // Turn off periodic allocations to avoid the race between
    // `HierarchicalAllocator::updateAvailable()` and periodic allocations.
    master::Flags flags = MasterTest::CreateMasterFlags();
    flags.allocation_interval = Seconds(1000);
    return flags;
  }

  Resources inboundResources(RepeatedPtrField<Resource> resources)
  {
    // If reservation refinement is enabled, inbound resources are already
    // in the "post-reservation-refinement" format and should not need to
    // be upgraded.
    if (GetParam()) {
      return resources;
    }

    convertResourceFormat(&resources, POST_RESERVATION_REFINEMENT);
    return resources;
  }

  RepeatedPtrField<Resource> outboundResources(
      RepeatedPtrField<Resource> resources)
  {
    // If reservation refinement is enabled, outbound resources are already
    // in the "post-reservation-refinement" format and should not need to
    // be downgraded.
    if (GetParam()) {
      return resources;
    }

    CHECK_SOME(downgradeResources(&resources));
    return resources;
  }
};


// Parameterized on reservation-refinement.
INSTANTIATE_TEST_CASE_P(
    bool,
    MasterTestPrePostReservationRefinement,
    ::testing::Values(true, false));


// This tests that a framework can launch a task with
// and without the RESERVATION_REFINEMENT capability.
TEST_P(MasterTestPrePostReservationRefinement, LaunchTask)
{
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_roles(0, DEFAULT_TEST_ROLE);

  // TODO(mpark): Remove this once `RESERVATION_REFINEMENT`
  // is removed from `DEFAULT_FRAMEWORK_INFO`.
  frameworkInfo.clear_capabilities();
  frameworkInfo.add_capabilities()->set_type(
      FrameworkInfo::Capability::MULTI_ROLE);

  if (GetParam()) {
    frameworkInfo.add_capabilities()->set_type(
        FrameworkInfo::Capability::RESERVATION_REFINEMENT);
  }

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), &containerizer);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());
  Offer offer = offers->front();

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offer.slave_id());
  task.mutable_resources()->MergeFrom(offer.resources());
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  EXPECT_CALL(exec, registered(_, _, _, _));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<Nothing> update;
  EXPECT_CALL(containerizer, update(_, inboundResources(offer.resources())))
    .WillOnce(DoAll(FutureSatisfy(&update), Return(Nothing())));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.launchTasks(offer.id(), {task});

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status->state());
  EXPECT_TRUE(status->has_executor_id());
  EXPECT_EQ(exec.id, status->executor_id());

  AWAIT_READY(update);

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();
}


// This tests that a framework can launch a task group
// with and without the RESERVATION_REFINEMENT capability.
TEST_P(MasterTestPrePostReservationRefinement, LaunchGroup)
{
  v1::FrameworkInfo frameworkInfo = v1::DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_roles(0, DEFAULT_TEST_ROLE);

  // TODO(mpark): Remove this once `RESERVATION_REFINEMENT`
  // is removed from `DEFAULT_FRAMEWORK_INFO`.
  frameworkInfo.clear_capabilities();
  frameworkInfo.add_capabilities()->set_type(
      v1::FrameworkInfo::Capability::MULTI_ROLE);

  if (GetParam()) {
    frameworkInfo.add_capabilities()->set_type(
        v1::FrameworkInfo::Capability::RESERVATION_REFINEMENT);
  }

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
#ifndef USE_SSL_SOCKET
  // Disable operator API authentication for the default executor. Executor
  // authentication currently has SSL as a dependency, so we cannot require
  // executors to authenticate with the agent operator API if Mesos was not
  // built with SSL support.
  flags.authenticate_http_readwrite = false;
#endif // USE_SSL_SOCKET

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), flags);
  ASSERT_SOME(slave);

  auto scheduler = std::make_shared<v1::MockHTTPScheduler>();

  Future<Nothing> connected;
  EXPECT_CALL(*scheduler, connected(_))
    .WillOnce(FutureSatisfy(&connected));

  v1::scheduler::TestMesos mesos(
      master.get()->pid,
      ContentType::PROTOBUF,
      scheduler);

  AWAIT_READY(connected);

  Future<v1::scheduler::Event::Subscribed> subscribed;
  EXPECT_CALL(*scheduler, subscribed(_, _))
    .WillOnce(FutureArg<1>(&subscribed));

  Future<v1::scheduler::Event::Offers> offers;
  EXPECT_CALL(*scheduler, offers(_, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return());

  EXPECT_CALL(*scheduler, heartbeat(_))
    .WillRepeatedly(Return()); // Ignore heartbeats.

  {
    Call call;
    call.set_type(Call::SUBSCRIBE);
    Call::Subscribe* subscribe = call.mutable_subscribe();
    subscribe->mutable_framework_info()->CopyFrom(frameworkInfo);

    mesos.send(call);
  }

  AWAIT_READY(subscribed);
  v1::FrameworkID frameworkId(subscribed->framework_id());

  RepeatedPtrField<Resource> resources =
    Resources::parse("cpus:0.1;mem:32;disk:32").get();

  v1::ExecutorInfo executorInfo;
  executorInfo.set_type(v1::ExecutorInfo::DEFAULT);
  executorInfo.mutable_executor_id()->CopyFrom(v1::DEFAULT_EXECUTOR_ID);
  executorInfo.mutable_framework_id()->CopyFrom(frameworkId);
  executorInfo.mutable_resources()->CopyFrom(
      evolve<v1::Resource>(outboundResources(resources)));

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->offers().empty());

  const v1::Offer& offer = offers->offers(0);
  const v1::AgentID& agentId = offer.agent_id();

  v1::TaskInfo taskInfo = v1::createTask(
      agentId, evolve<v1::Resource>(resources), SLEEP_COMMAND(1000));

  taskInfo.mutable_resources()->CopyFrom(evolve<v1::Resource>(
      outboundResources(devolve<Resource>(taskInfo.resources()))));

  v1::TaskGroupInfo taskGroup;
  taskGroup.add_tasks()->CopyFrom(taskInfo);

  Future<v1::scheduler::Event::Update> startingUpdate;
  Future<v1::scheduler::Event::Update> runningUpdate;
  EXPECT_CALL(*scheduler, update(_, _))
    .WillOnce(FutureArg<1>(&startingUpdate))
    .WillOnce(FutureArg<1>(&runningUpdate));

  {
    Call call;
    call.mutable_framework_id()->CopyFrom(frameworkId);
    call.set_type(Call::ACCEPT);

    Call::Accept* accept = call.mutable_accept();
    accept->add_offer_ids()->CopyFrom(offer.id());

    v1::Offer::Operation* operation = accept->add_operations();
    operation->set_type(v1::Offer::Operation::LAUNCH_GROUP);

    v1::Offer::Operation::LaunchGroup* launchGroup =
      operation->mutable_launch_group();

    launchGroup->mutable_executor()->CopyFrom(executorInfo);
    launchGroup->mutable_task_group()->CopyFrom(taskGroup);

    mesos.send(call);
  }

  AWAIT_READY(startingUpdate);

  EXPECT_EQ(TASK_STARTING, startingUpdate->status().state());
  EXPECT_EQ(taskInfo.task_id(), startingUpdate->status().task_id());
  EXPECT_TRUE(startingUpdate->status().has_timestamp());

  AWAIT_READY(runningUpdate);

  EXPECT_EQ(TASK_STARTING, runningUpdate->status().state());
  EXPECT_EQ(taskInfo.task_id(), runningUpdate->status().task_id());
  EXPECT_TRUE(runningUpdate->status().has_timestamp());

  // Ensure that the task sandbox symbolic link is created.
  EXPECT_TRUE(os::exists(path::join(
      slave::paths::getExecutorLatestRunPath(
          flags.work_dir,
          devolve(agentId),
          devolve(frameworkId),
          devolve(executorInfo.executor_id())),
      "tasks",
      taskInfo.task_id().value())));

  // Verify that the executor's type is exposed in the agent's state
  // endpoint.
  Future<Response> response = process::http::get(
      slave.get()->pid,
      "state",
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);
  AWAIT_EXPECT_RESPONSE_HEADER_EQ(APPLICATION_JSON, "Content-Type", response);

  Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
  ASSERT_SOME(parse);
  JSON::Object state = parse.get();

  EXPECT_SOME_EQ(
      JSON::String(v1::ExecutorInfo::Type_Name(executorInfo.type())),
      state.find<JSON::String>("frameworks[0].executors[0].type"));
}


// This tests that a framework can perform the operations
// RESERVE, CREATE, DESTROY, and UNRESERVE in that order
// with and without the RESERVATION_REFINEMENT capability.
TEST_P(MasterTestPrePostReservationRefinement,
       ReserveCreateLaunchDestroyUnreserve)
{
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_roles(0, DEFAULT_TEST_ROLE);

  // TODO(mpark): Remove this once `RESERVATION_REFINEMENT`
  // is removed from `DEFAULT_FRAMEWORK_INFO`.
  frameworkInfo.clear_capabilities();
  frameworkInfo.add_capabilities()->set_type(
      FrameworkInfo::Capability::MULTI_ROLE);

  if (GetParam()) {
    frameworkInfo.add_capabilities()->set_type(
        FrameworkInfo::Capability::RESERVATION_REFINEMENT);
  }

  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.allocation_interval = Milliseconds(5);
  masterFlags.roles = frameworkInfo.roles(0);

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "cpus:8;disk:512";

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  // We use the filter explicitly here so that the resources will not
  // be filtered for 5 seconds (the default).
  Filters filters;
  filters.set_refuse_seconds(0);

  Resources unreservedCpus = Resources::parse("cpus:8").get();
  Resources unreservedDisk = Resources::parse("disk:512").get();

  Resources reservedCpus =
    unreservedCpus.pushReservation(createDynamicReservationInfo(
        frameworkInfo.roles(0), frameworkInfo.principal()));

  Resources reservedDisk =
    unreservedDisk.pushReservation(createDynamicReservationInfo(
        frameworkInfo.roles(0), frameworkInfo.principal()));

  Resources volume = createPersistentVolume(
      createDiskResource("512", DEFAULT_TEST_ROLE, None(), None()),
      "id1",
      "path1",
      frameworkInfo.principal(),
      frameworkInfo.principal());

  // We use this to capture offers from 'resourceOffers'.
  Future<vector<Offer>> offers;

  EXPECT_CALL(sched, registered(&driver, _, _));

  // The expectation for the first offer.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  // In the first offer, expect an offer with unreserved resources.
  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  Offer offer = offers->front();

  EXPECT_TRUE(inboundResources(offer.resources())
                .contains(allocatedResources(
                    unreservedCpus + unreservedDisk, frameworkInfo.roles(0))));

  // The expectation for the next offer.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  // We don't use the `RESERVE` helper function here currently because it
  // takes `Resources` as its parameter, and the result of `outboundResources`
  // may be in the "pre-reservation-refinement" format.
  Offer::Operation reserve;
  reserve.set_type(Offer::Operation::RESERVE);
  reserve.mutable_reserve()->mutable_resources()->CopyFrom(
      outboundResources(reservedCpus + reservedDisk));

  // Reserve the resources.
  driver.acceptOffers({offer.id()}, {reserve}, filters);

  // In the next offer, expect an offer with reserved resources.
  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  offer = offers->front();

  EXPECT_TRUE(inboundResources(offer.resources())
                .contains(allocatedResources(
                    reservedCpus + reservedDisk, frameworkInfo.roles(0))));

  // The expectation for the next offer.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  // We don't use the `CREATE` helper function here currently because it
  // takes `Resources` as its parameter, and the result of `outboundResources`
  // may be in the "pre-reservation-refinement" format.
  Offer::Operation create;
  create.set_type(Offer::Operation::CREATE);
  create.mutable_create()->mutable_volumes()->CopyFrom(
      outboundResources(volume));

  // Create a volume.
  driver.acceptOffers({offer.id()}, {create}, filters);

  // In the next offer, expect an offer with reserved resources.
  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  offer = offers->front();

  EXPECT_TRUE(inboundResources(offer.resources())
                .contains(allocatedResources(
                    reservedCpus + volume, frameworkInfo.roles(0))));

  // The expectation for the next offer.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  // We don't use the `DESTROY` helper function here currently because it
  // takes `Resources` as its parameter, and the result of `outboundResources`
  // may be in the "pre-reservation-refinement" format.
  Offer::Operation destroy;
  destroy.set_type(Offer::Operation::DESTROY);
  destroy.mutable_destroy()->mutable_volumes()->CopyFrom(
      outboundResources(volume));

  // Destroy the volume.
  driver.acceptOffers({offer.id()}, {destroy}, filters);

  // In the next offer, expect an offer with unreserved resources.
  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  offer = offers.get()[0];

  EXPECT_TRUE(inboundResources(offer.resources())
                .contains(allocatedResources(
                    reservedCpus + reservedDisk, frameworkInfo.roles(0))));

  // The expectation for the next offer.
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  // We don't use the `UNRESERVE` helper function here currently because it
  // takes `Resources` as its parameter, and the result of `outboundResources`
  // may be in the "pre-reservation-refinement" format.
  Offer::Operation unreserve;
  unreserve.set_type(Offer::Operation::UNRESERVE);
  unreserve.mutable_unreserve()->mutable_resources()->CopyFrom(
      outboundResources(reservedCpus + reservedDisk));

  // Unreserve the resources.
  driver.acceptOffers({offer.id()}, {unreserve}, filters);

  // In the next offer, expect an offer with unreserved resources.
  AWAIT_READY(offers);

  ASSERT_EQ(1u, offers->size());
  offer = offers.get()[0];

  EXPECT_TRUE(inboundResources(offer.resources())
                .contains(allocatedResources(
                    unreservedCpus + unreservedDisk, frameworkInfo.roles(0))));

  driver.stop();
  driver.join();
}


// This test verifies that hitting the `/state` endpoint before '_accept()'
// is called results in pending tasks being reported correctly.
TEST_P(MasterTestPrePostReservationRefinement, StateEndpointPendingTasks)
{
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_roles(0, DEFAULT_TEST_ROLE);

  // TODO(mpark): Remove this once `RESERVATION_REFINEMENT`
  // is removed from `DEFAULT_FRAMEWORK_INFO`.
  frameworkInfo.clear_capabilities();
  frameworkInfo.add_capabilities()->set_type(
      FrameworkInfo::Capability::MULTI_ROLE);

  if (GetParam()) {
    frameworkInfo.add_capabilities()->set_type(
        FrameworkInfo::Capability::RESERVATION_REFINEMENT);
  }

  MockAuthorizer authorizer;
  Try<Owned<cluster::Master>> master = StartMaster(&authorizer);
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  Offer offer = offers->front();

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offer.slave_id());
  task.mutable_resources()->MergeFrom(offer.resources());
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  // Return a pending future from authorizer.
  Future<Nothing> authorize;
  Promise<bool> promise;
  EXPECT_CALL(authorizer, authorized(_))
    .WillOnce(DoAll(FutureSatisfy(&authorize),
                    Return(promise.future())));

  driver.launchTasks(offer.id(), {task});

  // Wait until authorization is in progress.
  AWAIT_READY(authorize);

  Future<Response> response = process::http::get(
      master.get()->pid,
      "state",
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(OK().status, response);

  Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
  ASSERT_SOME(parse);

  JSON::Value result = parse.get();

  JSON::Object expected = {
    {
      "frameworks",
      JSON::Array {
        JSON::Object {
          {
            "tasks",
            JSON::Array {
              JSON::Object {
                { "id", "1" },
                { "role", frameworkInfo.roles(0) },
                { "state", "TASK_STAGING" }
              }
            }
          }
        }
      }
    }
  };

  EXPECT_TRUE(result.contains(expected));

  driver.stop();
  driver.join();
}


// This test verifies that an operator can reserve and unreserve
// resources through the master operator API in both
// "(pre|post)-reservation-refinement" formats.
TEST_P(MasterTestPrePostReservationRefinement, ReserveAndUnreserveResourcesV1)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  // For capturing the SlaveID.
  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);
  SlaveID slaveId = slaveRegisteredMessage->slave_id();

  v1::master::Call v1ReserveResourcesCall;
  v1ReserveResourcesCall.set_type(v1::master::Call::RESERVE_RESOURCES);
  v1::master::Call::ReserveResources* reserveResources =
    v1ReserveResourcesCall.mutable_reserve_resources();

  Resources unreserved = Resources::parse("cpus:1;mem:512").get();
  Resources dynamicallyReserved =
    unreserved.pushReservation(createDynamicReservationInfo(
        DEFAULT_TEST_ROLE, DEFAULT_CREDENTIAL.principal()));

  reserveResources->mutable_agent_id()->CopyFrom(evolve(slaveId));
  reserveResources->mutable_resources()->CopyFrom(
      evolve<v1::Resource>(outboundResources(dynamicallyReserved)));

  ContentType contentType = ContentType::PROTOBUF;

  Future<Response> v1ReserveResourcesResponse = process::http::post(
    master.get()->pid,
    "api/v1",
    createBasicAuthHeaders(DEFAULT_CREDENTIAL),
    serialize(contentType, v1ReserveResourcesCall),
    stringify(contentType));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(
      Accepted().status, v1ReserveResourcesResponse);

  v1::master::Call v1UnreserveResourcesCall;
  v1UnreserveResourcesCall.set_type(v1::master::Call::UNRESERVE_RESOURCES);
  v1::master::Call::UnreserveResources* unreserveResources =
    v1UnreserveResourcesCall.mutable_unreserve_resources();

  unreserveResources->mutable_agent_id()->CopyFrom(evolve(slaveId));

  unreserveResources->mutable_resources()->CopyFrom(
      evolve<v1::Resource>(outboundResources(dynamicallyReserved)));

  Future<Response> v1UnreserveResourcesResponse = process::http::post(
      master.get()->pid,
      "api/v1",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      serialize(contentType, v1UnreserveResourcesCall),
      stringify(contentType));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(
      Accepted().status, v1UnreserveResourcesResponse);
}


// This test verifies that an operator can create and destroy
// persistent volumes through the master operator API in both
// "(pre|post)-reservation-refinement" formats.
TEST_P(MasterTestPrePostReservationRefinement, CreateAndDestroyVolumesV1)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  // For capturing the SlaveID so we can use it in the create/destroy volumes
  // API call.
  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), _, _);

  Owned<MasterDetector> detector = master.get()->createDetector();

  slave::Flags slaveFlags = CreateSlaveFlags();
  // Do Static reservation so we can create persistent volumes from it.
  slaveFlags.resources = "disk(role1):1024";

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);

  ASSERT_SOME(slave);

  AWAIT_READY(slaveRegisteredMessage);
  SlaveID slaveId = slaveRegisteredMessage->slave_id();

  // Create the persistent volume.
  v1::master::Call v1CreateVolumesCall;
  v1CreateVolumesCall.set_type(v1::master::Call::CREATE_VOLUMES);
  v1::master::Call_CreateVolumes* createVolumes =
    v1CreateVolumesCall.mutable_create_volumes();

  Resources volume = createPersistentVolume(
      Megabytes(64),
      "role1",
      "id1",
      "path1",
      None(),
      None(),
      DEFAULT_CREDENTIAL.principal());

  createVolumes->mutable_agent_id()->CopyFrom(evolve(slaveId));
  createVolumes->mutable_volumes()->CopyFrom(
      evolve<v1::Resource>(outboundResources(volume)));

  ContentType contentType = ContentType::PROTOBUF;

  Future<Response> v1CreateVolumesResponse = process::http::post(
      master.get()->pid,
      "api/v1",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      serialize(contentType, v1CreateVolumesCall),
      stringify(contentType));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Accepted().status, v1CreateVolumesResponse);

  // Destroy the persistent volume.
  v1::master::Call v1DestroyVolumesCall;
  v1DestroyVolumesCall.set_type(v1::master::Call::DESTROY_VOLUMES);
  v1::master::Call_DestroyVolumes* destroyVolumes =
    v1DestroyVolumesCall.mutable_destroy_volumes();

  destroyVolumes->mutable_agent_id()->CopyFrom(evolve(slaveId));
  destroyVolumes->mutable_volumes()->CopyFrom(
      evolve<v1::Resource>(outboundResources(volume)));

  Future<Response> v1DestroyVolumesResponse = process::http::post(
      master.get()->pid,
      "api/v1",
      createBasicAuthHeaders(DEFAULT_CREDENTIAL),
      serialize(contentType, v1DestroyVolumesCall),
      stringify(contentType));

  AWAIT_EXPECT_RESPONSE_STATUS_EQ(Accepted().status, v1DestroyVolumesResponse);
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
