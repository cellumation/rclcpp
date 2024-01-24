// Copyright 2017 Open Source Robotics Foundation, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/**
 * This test checks all implementations of rclcpp::executor to check they pass they basic API
 * tests. Anything specific to any executor in particular should go in a separate test file.
 *
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "rcl/error_handling.h"
#include "rcl/time.h"
#include "rclcpp/clock.hpp"
#include "rclcpp/detail/add_guard_condition_to_rcl_wait_set.hpp"
#include "rclcpp/duration.hpp"
#include "rclcpp/guard_condition.hpp"
#include "rclcpp/rclcpp.hpp"

#include "test_msgs/msg/empty.hpp"

using namespace std::chrono_literals;


template<typename T>
class TestExecutorsOnlyNode : public ::testing::Test
{
public:
  void SetUp()
  {
    rclcpp::init(0, nullptr);

    const auto test_info = ::testing::UnitTest::GetInstance()->current_test_info();
    std::stringstream test_name;
    test_name << test_info->test_case_name() << "_" << test_info->name();
    node = std::make_shared<rclcpp::Node>("node", test_name.str());

  }

  void TearDown()
  {
    node.reset();

    rclcpp::shutdown();
  }

  rclcpp::Node::SharedPtr node;
};

template<typename T>
class TestExecutors : public ::testing::Test
{
public:
  void SetUp()
  {
    rclcpp::init(0, nullptr);

    const auto test_info = ::testing::UnitTest::GetInstance()->current_test_info();
    std::stringstream test_name;
    test_name << test_info->test_case_name() << "_" << test_info->name();
    node = std::make_shared<rclcpp::Node>("node", test_name.str());

    callback_count = 0;

    const std::string topic_name = std::string("topic_") + test_name.str();
    publisher = node->create_publisher<test_msgs::msg::Empty>(topic_name, rclcpp::QoS(10));
    auto callback = [this](test_msgs::msg::Empty::ConstSharedPtr) {this->callback_count++;};
    subscription =
      node->create_subscription<test_msgs::msg::Empty>(
      topic_name, rclcpp::QoS(10), std::move(callback));
  }

  void TearDown()
  {
    publisher.reset();
    subscription.reset();
    node.reset();

    rclcpp::shutdown();
  }

  rclcpp::Node::SharedPtr node;
  rclcpp::Publisher<test_msgs::msg::Empty>::SharedPtr publisher;
  rclcpp::Subscription<test_msgs::msg::Empty>::SharedPtr subscription;
  int callback_count;
};

// spin_all and spin_some are not implemented correctly in StaticSingleThreadedExecutor, see:
// https://github.com/ros2/rclcpp/issues/1219 for tracking
template<typename T>
class TestExecutorsStable : public TestExecutors<T> {};

using ExecutorTypes =
  ::testing::Types<
  rclcpp::executors::SingleThreadedExecutor,
  rclcpp::executors::MultiThreadedExecutor,
  rclcpp::executors::StaticSingleThreadedExecutor,
  rclcpp::experimental::executors::EventsExecutor>;

class ExecutorTypeNames
{
public:
  template<typename T>
  static std::string GetName(int idx)
  {
    (void)idx;
    if (std::is_same<T, rclcpp::executors::SingleThreadedExecutor>()) {
      return "SingleThreadedExecutor";
    }

    if (std::is_same<T, rclcpp::executors::MultiThreadedExecutor>()) {
      return "MultiThreadedExecutor";
    }

    if (std::is_same<T, rclcpp::executors::StaticSingleThreadedExecutor>()) {
      return "StaticSingleThreadedExecutor";
    }

    if (std::is_same<T, rclcpp::experimental::executors::EventsExecutor>()) {
      return "EventsExecutor";
    }

    return "";
  }
};

// TYPED_TEST_SUITE is deprecated as of gtest 1.9, use TYPED_TEST_SUITE when gtest dependency
// is updated.
TYPED_TEST_SUITE(TestExecutors, ExecutorTypes, ExecutorTypeNames);

TYPED_TEST_SUITE(TestExecutorsOnlyNode, ExecutorTypes, ExecutorTypeNames);

// StaticSingleThreadedExecutor is not included in these tests for now, due to:
// https://github.com/ros2/rclcpp/issues/1219
using StandardExecutors =
  ::testing::Types<
  rclcpp::executors::SingleThreadedExecutor,
  rclcpp::executors::MultiThreadedExecutor,
  rclcpp::experimental::executors::EventsExecutor>;
TYPED_TEST_SUITE(TestExecutorsStable, StandardExecutors, ExecutorTypeNames);

// Make sure that executors detach from nodes when destructing
TYPED_TEST(TestExecutors, detachOnDestruction)
{
  using ExecutorType = TypeParam;
  {
    ExecutorType executor;
    executor.add_node(this->node);
  }
  {
    ExecutorType executor;
    EXPECT_NO_THROW(executor.add_node(this->node));
  }
}

// Make sure that the executor can automatically remove expired nodes correctly
// Currently fails for StaticSingleThreadedExecutor so it is being skipped, see:
// https://github.com/ros2/rclcpp/issues/1231
TYPED_TEST(TestExecutorsStable, addTemporaryNode)
{
  using ExecutorType = TypeParam;
  ExecutorType executor;

  {
    // Let node go out of scope before executor.spin()
    auto node = std::make_shared<rclcpp::Node>("temporary_node");
    executor.add_node(node);
  }

  // Sleep for a short time to verify executor.spin() is going, and didn't throw.
  std::thread spinner([&]() {EXPECT_NO_THROW(executor.spin());});

  std::this_thread::sleep_for(50ms);
  executor.cancel();
  spinner.join();
}

// Make sure that a spinning empty executor can be cancelled
TYPED_TEST(TestExecutors, emptyExecutor)
{
  using ExecutorType = TypeParam;
  ExecutorType executor;
  std::thread spinner([&]() {EXPECT_NO_THROW(executor.spin());});
  std::this_thread::sleep_for(50ms);
  executor.cancel();
  spinner.join();
}

// Check executor throws properly if the same node is added a second time
TYPED_TEST(TestExecutors, addNodeTwoExecutors)
{
  using ExecutorType = TypeParam;
  ExecutorType executor1;
  ExecutorType executor2;
  EXPECT_NO_THROW(executor1.add_node(this->node));
  EXPECT_THROW(executor2.add_node(this->node), std::runtime_error);
  executor1.remove_node(this->node, true);
}

// Check simple spin example
TYPED_TEST(TestExecutors, spinWithTimer)
{
  using ExecutorType = TypeParam;
  ExecutorType executor;

  bool timer_completed = false;
  auto timer = this->node->create_wall_timer(1ms, [&]() {timer_completed = true;});
  executor.add_node(this->node);

  std::thread spinner([&]() {executor.spin();});

  auto start = std::chrono::steady_clock::now();
  while (!timer_completed && (std::chrono::steady_clock::now() - start) < 10s) {
    std::this_thread::sleep_for(1ms);
  }

  EXPECT_TRUE(timer_completed);
  // Cancel needs to be called before join, so that executor.spin() returns.
  executor.cancel();
  spinner.join();
  executor.remove_node(this->node, true);
}

TYPED_TEST(TestExecutors, spinWhileAlreadySpinning)
{
  using ExecutorType = TypeParam;
  ExecutorType executor;
  executor.add_node(this->node);

  bool timer_completed = false;
  auto timer = this->node->create_wall_timer(1ms, [&]() {timer_completed = true;});

  std::thread spinner([&]() {executor.spin();});
  // Sleep for a short time to verify executor.spin() is going, and didn't throw.

  auto start = std::chrono::steady_clock::now();
  while (!timer_completed && (std::chrono::steady_clock::now() - start) < 10s) {
    std::this_thread::sleep_for(1ms);
  }

  EXPECT_TRUE(timer_completed);
  EXPECT_THROW(executor.spin(), std::runtime_error);

  // Shutdown needs to be called before join, so that executor.spin() returns.
  executor.cancel();
  spinner.join();
  executor.remove_node(this->node, true);
}

// Check executor exits immediately if future is complete.
TYPED_TEST(TestExecutors, testSpinUntilFutureComplete)
{
  using ExecutorType = TypeParam;
  ExecutorType executor;
  executor.add_node(this->node);

  // test success of an immediately finishing future
  std::promise<bool> promise;
  std::future<bool> future = promise.get_future();
  promise.set_value(true);

  // spin_until_future_complete is expected to exit immediately, but would block up until its
  // timeout if the future is not checked before spin_once_impl.
  auto start = std::chrono::steady_clock::now();
  auto shared_future = future.share();
  auto ret = executor.spin_until_future_complete(shared_future, 1s);
  executor.remove_node(this->node, true);
  // Check it didn't reach timeout
  EXPECT_GT(500ms, (std::chrono::steady_clock::now() - start));
  EXPECT_EQ(rclcpp::FutureReturnCode::SUCCESS, ret);
}

// Same test, but uses a shared future.
TYPED_TEST(TestExecutors, testSpinUntilSharedFutureComplete)
{
  using ExecutorType = TypeParam;
  ExecutorType executor;
  executor.add_node(this->node);

  // test success of an immediately finishing future
  std::promise<bool> promise;
  std::future<bool> future = promise.get_future();
  promise.set_value(true);

  // spin_until_future_complete is expected to exit immediately, but would block up until its
  // timeout if the future is not checked before spin_once_impl.
  auto shared_future = future.share();
  auto start = std::chrono::steady_clock::now();
  auto ret = executor.spin_until_future_complete(shared_future, 1s);
  executor.remove_node(this->node, true);

  // Check it didn't reach timeout
  EXPECT_GT(500ms, (std::chrono::steady_clock::now() - start));
  EXPECT_EQ(rclcpp::FutureReturnCode::SUCCESS, ret);
}

// For a longer running future that should require several iterations of spin_once
TYPED_TEST(TestExecutors, testSpinUntilFutureCompleteNoTimeout)
{
  using ExecutorType = TypeParam;
  ExecutorType executor;
  executor.add_node(this->node);

  // This future doesn't immediately terminate, so some work gets performed.
  std::future<void> future = std::async(
    std::launch::async,
    [this]() {
      auto start = std::chrono::steady_clock::now();
      while (this->callback_count < 1 && (std::chrono::steady_clock::now() - start) < 1s) {
        std::this_thread::sleep_for(1ms);
      }
    });

  bool spin_exited = false;

  // Timeout set to negative for no timeout.
  std::thread spinner([&]() {
      auto ret = executor.spin_until_future_complete(future, -1s);
      EXPECT_EQ(rclcpp::FutureReturnCode::SUCCESS, ret);
      executor.remove_node(this->node, true);
      executor.cancel();
      spin_exited = true;
    });

  // Do some work for longer than the future needs.
  for (int i = 0; i < 100; ++i) {
    this->publisher->publish(test_msgs::msg::Empty());
    std::this_thread::sleep_for(1ms);
    if (spin_exited) {
      break;
    }
  }

  // Not testing accuracy, just want to make sure that some work occurred.
  EXPECT_LT(0, this->callback_count);

  // If this fails, the test will probably crash because spinner goes out of scope while the thread
  // is active. However, it beats letting this run until the gtest timeout.
  ASSERT_TRUE(spin_exited);
  executor.cancel();
  spinner.join();
}

// Check spin_until_future_complete timeout works as expected
TYPED_TEST(TestExecutors, testSpinUntilFutureCompleteWithTimeout)
{
  using ExecutorType = TypeParam;
  ExecutorType executor;
  executor.add_node(this->node);

  bool spin_exited = false;

  // Needs to run longer than spin_until_future_complete's timeout.
  std::future<void> future = std::async(
    std::launch::async,
    [&spin_exited]() {
      auto start = std::chrono::steady_clock::now();
      while (!spin_exited && (std::chrono::steady_clock::now() - start) < 1s) {
        std::this_thread::sleep_for(1ms);
      }
    });

  // Short timeout
  std::thread spinner([&]() {
      auto ret = executor.spin_until_future_complete(future, 1ms);
      EXPECT_EQ(rclcpp::FutureReturnCode::TIMEOUT, ret);
      executor.remove_node(this->node, true);
      spin_exited = true;
    });

  // Do some work for longer than timeout needs.
  for (int i = 0; i < 100; ++i) {
    this->publisher->publish(test_msgs::msg::Empty());
    std::this_thread::sleep_for(1ms);
    if (spin_exited) {
      break;
    }
  }

  EXPECT_TRUE(spin_exited);
  spinner.join();
}

class TestWaitable : public rclcpp::Waitable
{
public:
  TestWaitable() = default;

  void
  add_to_wait_set(rcl_wait_set_t * wait_set) override
  {
    rclcpp::detail::add_guard_condition_to_rcl_wait_set(*wait_set, gc_);
    if (retrigger_guard_condition && num_unprocessed_triggers > 0) {
      gc_.trigger();
    }
  }

  void trigger()
  {
    num_unprocessed_triggers++;
    gc_.trigger();
  }

  void trigger_and_hold_execute()
  {
    hold_execute = true;

    trigger();
  }

  void release_execute()
  {
    hold_execute = false;
    cv.notify_one();
  }

  bool
  is_ready(rcl_wait_set_t * wait_set) override
  {
    is_ready_called_before_take_data = true;
    for (size_t i = 0; i < wait_set->size_of_guard_conditions; ++i) {
      if (&gc_.get_rcl_guard_condition() == wait_set->guard_conditions[i]) {
        return true;
      }
    }
    return false;
  }

  std::shared_ptr<void>
  take_data() override
  {
    if (!is_ready_called_before_take_data) {
      throw std::runtime_error(
              "TestWaitable : Internal error, take data was called, but is_ready was not called before");
    }

    is_ready_called_before_take_data = false;

    num_unprocessed_triggers--;

    return nullptr;
  }

  std::shared_ptr<void>
  take_data_by_entity_id(size_t id) override
  {
    (void) id;
    return take_data();
  }

  void
  execute(std::shared_ptr<void> & data) override
  {
    (void) data;
    count_++;
    std::this_thread::sleep_for(3ms);
    try {
      std::lock_guard<std::mutex> lock(execute_promise_mutex_);
      execute_promise_.set_value();
    } catch (const std::future_error & future_error) {
      if (future_error.code() != std::future_errc::promise_already_satisfied) {
        throw;
      }
    }

    if(hold_execute)
    {
      std::unique_lock<std::mutex> lk(cv_m);
      cv.wait(lk);
    }
  }

  void
  set_on_ready_callback(std::function<void(size_t, int)> callback) override
  {
    auto gc_callback = [callback](size_t count) {
        callback(count, 0);
      };
    gc_.set_on_trigger_callback(gc_callback);
  }

  void
  clear_on_ready_callback() override
  {
    gc_.set_on_trigger_callback(nullptr);
  }

  size_t
  get_number_of_ready_guard_conditions() override {return 1;}

  size_t
  get_count()
  {
    return count_;
  }

  std::future<void>
  reset_execute_promise_and_get_future()
  {
    std::lock_guard<std::mutex> lock(execute_promise_mutex_);
    execute_promise_ = std::promise<void>();
    return execute_promise_.get_future();
  }

  void enable_retriggering(bool enabled)
  {
    retrigger_guard_condition = enabled;
  }

private:
  bool is_ready_called_before_take_data = false;
  bool retrigger_guard_condition = true;
  std::promise<void> execute_promise_;
  std::mutex execute_promise_mutex_;
  std::atomic<uint> num_unprocessed_triggers = 0;
  std::atomic<bool> hold_execute = false;
  size_t count_ = 0;
  std::condition_variable cv;
  std::mutex cv_m;

  rclcpp::GuardCondition gc_;
};


TYPED_TEST(TestExecutors, spinAll)
{
  using ExecutorType = TypeParam;
  ExecutorType executor;
  auto waitable_interfaces = this->node->get_node_waitables_interface();
  auto my_waitable = std::make_shared<TestWaitable>();
  waitable_interfaces->add_waitable(my_waitable, nullptr);
  executor.add_node(this->node);

  // Long timeout, but should not block test if spin_all works as expected as we cancel the
  // executor.
  bool spin_exited = false;
  std::thread spinner([&spin_exited, &executor, this]() {
      executor.spin_all(1s);
      executor.remove_node(this->node, true);
      spin_exited = true;
    });

  // Do some work until sufficient calls to the waitable occur
  auto start = std::chrono::steady_clock::now();
  while (
    my_waitable->get_count() <= 1 &&
    !spin_exited &&
    (std::chrono::steady_clock::now() - start < 1s))
  {
    my_waitable->trigger();
    this->publisher->publish(test_msgs::msg::Empty());
    std::this_thread::sleep_for(1ms);
  }

  executor.cancel();
  start = std::chrono::steady_clock::now();
  while (!spin_exited && (std::chrono::steady_clock::now() - start) < 1s) {
    std::this_thread::sleep_for(1ms);
  }

  EXPECT_LT(1u, my_waitable->get_count());
  waitable_interfaces->remove_waitable(my_waitable, nullptr);
  ASSERT_TRUE(spin_exited);
  spinner.join();
}

TEST(TestExecutors, double_take_data)
{
  rclcpp::init(0, nullptr);

  rclcpp::executors::MultiThreadedExecutor executor;

  const auto test_info = ::testing::UnitTest::GetInstance()->current_test_info();
  std::stringstream test_name;
  test_name << test_info->test_case_name() << "_" << test_info->name();
  rclcpp::Node::SharedPtr node = std::make_shared<rclcpp::Node>("node", test_name.str());

  auto waitable_interfaces = node->get_node_waitables_interface();

  auto first_cbg = node->create_callback_group(
    rclcpp::CallbackGroupType::MutuallyExclusive,
    true);

  auto third_cbg = node->create_callback_group(
    rclcpp::CallbackGroupType::MutuallyExclusive,
    true);

  // these waitable have one job, to make the MemoryStrategy::collect_enties method take
  // a long time, in order to force our race condition
  std::vector<std::shared_ptr<TestWaitable>> stuffing_waitables;
  std::vector<std::shared_ptr<rclcpp::CallbackGroup>> stuffing_cbgs;

  for (size_t i = 0; i < 50; i++) {
    auto cbg = node->create_callback_group(
      rclcpp::CallbackGroupType::MutuallyExclusive,
      true);

    stuffing_cbgs.push_back(cbg);

    for (int j = 0; j < 200; j++) {
      auto waitable = std::make_shared<TestWaitable>();
      stuffing_waitables.push_back(waitable);
      waitable_interfaces->add_waitable(waitable, cbg);
    }
  }

  // this is the callback group were wo introduce the double take
  auto callback_group = node->create_callback_group(
    rclcpp::CallbackGroupType::MutuallyExclusive,
    true);


  std::vector<std::shared_ptr<TestWaitable>> waitables;

  auto w3 = std::make_shared<TestWaitable>();
  waitable_interfaces->add_waitable(w3, third_cbg);

  // First group of waitables, that gets processed.
  // We use the shared count of these waitables and the callback group,
  // to estimate when MemoryStrategy::collect_enties is called in the spinner thread
  std::vector<std::shared_ptr<TestWaitable>> first_waitables;
  auto non_triggered_in_first_cbg = std::make_shared<TestWaitable>();
  waitable_interfaces->add_waitable(non_triggered_in_first_cbg, first_cbg);

  auto non_triggered_in_first_cbg2 = std::make_shared<TestWaitable>();
  waitable_interfaces->add_waitable(non_triggered_in_first_cbg2, first_cbg);


  auto cbg_start = std::make_shared<TestWaitable>();
  waitable_interfaces->add_waitable(cbg_start, callback_group);

  // These waitables will get triggered while cbg_start is beeing executed
  for (int i = 0; i < 20; i++) {
    auto waitable = std::make_shared<TestWaitable>();
    waitables.push_back(waitable);
    waitable_interfaces->add_waitable(waitable, callback_group);
  }

  // used to detect if all triggers were processed
  auto cbg_end = std::make_shared<TestWaitable>();
  waitable_interfaces->add_waitable(cbg_end, callback_group);

  executor.add_node(node);

  // ref count if not reference by the internals of the executor
  auto min_ref_cnt = non_triggered_in_first_cbg.use_count();
  auto cbg_min_ref_cnt = first_cbg.use_count();

  for (auto & w : waitables) {
    EXPECT_EQ(w->get_count(), 0);
  }

  std::atomic_bool exception = false;

  std::thread t([&executor, &exception]() {
      try {
        executor.spin();
      } catch (const std::exception & e) {
        exception = true;
      } catch (...) {
        exception = true;
      }
    });

  size_t start_count = cbg_start->get_count();
  cbg_start->trigger_and_hold_execute();

  //wait until the first waitable is executed and blocks the callback_group
  while (cbg_start->get_count() == start_count) {
    std::this_thread::sleep_for(1ms);
  }

  for (auto & w : waitables) {
    w->trigger();
  }

  // trigger w3 to make sure, the MemoryStrategy clears its internal list of ready entities
  {
    auto cnt = w3->get_count();
    w3->trigger();
    while (w3->get_count() == cnt) {
      std::this_thread::sleep_for(1ms);
    }
  }

  // observe the use counts of non_triggered_in_first_cbg, non_triggered_in_first_cbg2 and first_cbg
  // in order to fugure out if MemoryStrategy::collect_enties is beeing calles
  while (true) {
    w3->trigger();
    bool restart = false;

    // There should be no reference to our waitiable
    while (min_ref_cnt != non_triggered_in_first_cbg.use_count() ||
      min_ref_cnt != non_triggered_in_first_cbg2.use_count())
    {
    }

    // wait for the callback group to be taken
    while (true) {
      // node and callback group ptrs are referenced
      if (cbg_min_ref_cnt != first_cbg.use_count()) {
        break;
      }

      // is we got more references to the waitable, while the group pointer is not referenced, this is the wrong spot
      if (min_ref_cnt != non_triggered_in_first_cbg.use_count() ||
        min_ref_cnt != non_triggered_in_first_cbg2.use_count())
      {
        restart = true;
        break;
      }
    }

    if (restart) {
      continue;
    }

    // callback group pointer is referenced
    while (true) {
      // trigger criteria, both pointers were collected
      if (min_ref_cnt != non_triggered_in_first_cbg.use_count() &&
        min_ref_cnt != non_triggered_in_first_cbg2.use_count())
      {
        break;
      }

      // invalid, second pointer is referenced, but not first one
      if (min_ref_cnt == non_triggered_in_first_cbg.use_count() &&
        min_ref_cnt != non_triggered_in_first_cbg2.use_count())
      {
        restart = true;
        break;
      }

      // group or node pointer was released, while waitable poitner were not taken.
      if (cbg_min_ref_cnt == first_cbg.use_count()) {
        restart = true;
        break;
      }

    }
    if (restart) {
      continue;
    }

    break;
  }

  //we unblock the callback_group now, this should force the race condition
  cbg_start->release_execute();

  std::this_thread::yield();
  std::this_thread::sleep_for(10ms);

  size_t end_count = cbg_end->get_count();
  cbg_end->trigger();

  // wait for all triggers to be executed, or for an exception to occur
  while (end_count == cbg_end->get_count() && !exception) {
    std::this_thread::sleep_for(10ms);
  }

  EXPECT_FALSE(exception);

  node.reset();
  rclcpp::shutdown();

  t.join();
}

TYPED_TEST(TestExecutorsOnlyNode, missing_event)
{
  using ExecutorType = TypeParam;
  ExecutorType executor;

  rclcpp::Node::SharedPtr node(this->node);
  auto callback_group = node->create_callback_group(
    rclcpp::CallbackGroupType::MutuallyExclusive,
    false);

  std::chrono::seconds max_spin_duration(2);
  auto waitable_interfaces = node->get_node_waitables_interface();
  auto my_waitable = std::make_shared<TestWaitable>();
  auto my_waitable2 = std::make_shared<TestWaitable>();
  waitable_interfaces->add_waitable(my_waitable, callback_group);
  waitable_interfaces->add_waitable(my_waitable2, callback_group);
  executor.add_callback_group(callback_group, node->get_node_base_interface());

  my_waitable->trigger();
  my_waitable2->trigger();

  {
    auto my_waitable_execute_future = my_waitable->reset_execute_promise_and_get_future();
    executor.spin_until_future_complete(my_waitable_execute_future, max_spin_duration);
  }

  EXPECT_EQ(1u, my_waitable->get_count());
  EXPECT_EQ(0u, my_waitable2->get_count());

  // block the callback group, this is something that may happen during multi threaded execution
  // This removes my_waitable2 from the list of ready events, and triggers a call to wait_for_work
  callback_group->can_be_taken_from().exchange(false);

  // now there should be no ready event
  {
    auto my_waitable2_execute_future = my_waitable2->reset_execute_promise_and_get_future();
    auto future_code = executor.spin_until_future_complete(
      my_waitable2_execute_future,
      std::chrono::milliseconds(100));  // expected to timeout
    EXPECT_EQ(future_code, rclcpp::FutureReturnCode::TIMEOUT);
  }

  EXPECT_EQ(1u, my_waitable->get_count());
  EXPECT_EQ(0u, my_waitable2->get_count());

  // unblock the callback group
  callback_group->can_be_taken_from().exchange(true);

  // now the second waitable should get processed
  {
    auto my_waitable2_execute_future = my_waitable2->reset_execute_promise_and_get_future();
    executor.spin_until_future_complete(my_waitable2_execute_future, max_spin_duration);
  }

  EXPECT_EQ(1u, my_waitable->get_count());
  EXPECT_EQ(1u, my_waitable2->get_count());
  //now the second waitable should get processed
  executor.spin_once(std::chrono::milliseconds(10));

  EXPECT_EQ(1u, my_waitable->get_count());
  EXPECT_EQ(1u, my_waitable2->get_count());
}

TYPED_TEST(TestExecutors, spinSome)
{
  using ExecutorType = TypeParam;
  ExecutorType executor;
  auto waitable_interfaces = this->node->get_node_waitables_interface();
  auto my_waitable = std::make_shared<TestWaitable>();
  waitable_interfaces->add_waitable(my_waitable, nullptr);
  executor.add_node(this->node);

  // Long timeout, doesn't block test from finishing because spin_some should exit after the
  // first one completes.
  bool spin_exited = false;
  std::thread spinner([&spin_exited, &executor, this]() {
      executor.spin_some(1s);
      executor.remove_node(this->node, true);
      spin_exited = true;
    });

  // Do some work until sufficient calls to the waitable occur, but keep going until either
  // count becomes too large, spin exits, or the 1 second timeout completes.
  auto start = std::chrono::steady_clock::now();
  while (
    my_waitable->get_count() <= 1 &&
    !spin_exited &&
    (std::chrono::steady_clock::now() - start < 1s))
  {
    my_waitable->trigger();
    this->publisher->publish(test_msgs::msg::Empty());
    std::this_thread::sleep_for(1ms);
  }
  // The count of "execute" depends on whether the executor starts spinning before (1) or after (0)
  // the first iteration of the while loop
  EXPECT_LE(1u, my_waitable->get_count());
  waitable_interfaces->remove_waitable(my_waitable, nullptr);
  EXPECT_TRUE(spin_exited);
  // Cancel if it hasn't exited already.
  executor.cancel();

  spinner.join();
}

// Check spin_node_until_future_complete with node base pointer
TYPED_TEST(TestExecutors, testSpinNodeUntilFutureCompleteNodeBasePtr)
{
  using ExecutorType = TypeParam;
  ExecutorType executor;

  std::promise<bool> promise;
  std::future<bool> future = promise.get_future();
  promise.set_value(true);

  auto shared_future = future.share();
  auto ret = rclcpp::executors::spin_node_until_future_complete(
    executor, this->node->get_node_base_interface(), shared_future, 1s);
  EXPECT_EQ(rclcpp::FutureReturnCode::SUCCESS, ret);
}

// Check spin_node_until_future_complete with node pointer
TYPED_TEST(TestExecutors, testSpinNodeUntilFutureCompleteNodePtr)
{
  using ExecutorType = TypeParam;
  ExecutorType executor;

  std::promise<bool> promise;
  std::future<bool> future = promise.get_future();
  promise.set_value(true);

  auto shared_future = future.share();
  auto ret = rclcpp::executors::spin_node_until_future_complete(
    executor, this->node, shared_future, 1s);
  EXPECT_EQ(rclcpp::FutureReturnCode::SUCCESS, ret);
}

// Check spin_until_future_complete can be properly interrupted.
TYPED_TEST(TestExecutors, testSpinUntilFutureCompleteInterrupted)
{
  using ExecutorType = TypeParam;
  ExecutorType executor;
  executor.add_node(this->node);

  bool spin_exited = false;

  // This needs to block longer than it takes to get to the shutdown call below and for
  // spin_until_future_complete to return
  std::future<void> future = std::async(
    std::launch::async,
    [&spin_exited]() {
      auto start = std::chrono::steady_clock::now();
      while (!spin_exited && (std::chrono::steady_clock::now() - start) < 1s) {
        std::this_thread::sleep_for(1ms);
      }
    });

  // Long timeout
  std::thread spinner([&spin_exited, &executor, &future]() {
      auto ret = executor.spin_until_future_complete(future, 1s);
      EXPECT_EQ(rclcpp::FutureReturnCode::INTERRUPTED, ret);
      spin_exited = true;
    });

  // Do some minimal work
  this->publisher->publish(test_msgs::msg::Empty());
  std::this_thread::sleep_for(1ms);

  // Force interruption
  rclcpp::shutdown();

  // Give it time to exit
  auto start = std::chrono::steady_clock::now();
  while (!spin_exited && (std::chrono::steady_clock::now() - start) < 1s) {
    std::this_thread::sleep_for(1ms);
  }

  EXPECT_TRUE(spin_exited);
  spinner.join();
}

// This test verifies that the add_node operation is robust wrt race conditions.
// It's mostly meant to prevent regressions in the events-executor, but the operation should be
// thread-safe in all executor implementations.
// The initial implementation of the events-executor contained a bug where the executor
// would end up in an inconsistent state and stop processing interrupt/shutdown notifications.
// Manually adding a node to the executor results in a) producing a notify waitable event
// and b) refreshing the executor collections.
// The inconsistent state would happen if the event was processed before the collections were
// finished to be refreshed: the executor would pick up the event but be unable to process it.
// This would leave the `notify_waitable_event_pushed_` flag to true, preventing additional
// notify waitable events to be pushed.
// The behavior is observable only under heavy load, so this test spawns several worker
// threads. Due to the nature of the bug, this test may still succeed even if the
// bug is present. However repeated runs will show its flakiness nature and indicate
// an eventual regression.
TYPED_TEST(TestExecutors, testRaceConditionAddNode)
{
  using ExecutorType = TypeParam;
  // rmw_connextdds doesn't support events-executor
  if (
    std::is_same<ExecutorType, rclcpp::experimental::executors::EventsExecutor>() &&
    std::string(rmw_get_implementation_identifier()).find("rmw_connextdds") == 0)
  {
    GTEST_SKIP();
  }

  // Spawn some threads to do some heavy work
  std::atomic<bool> should_cancel = false;
  std::vector<std::thread> stress_threads;
  for (size_t i = 0; i < 5 * std::thread::hardware_concurrency(); i++) {
    stress_threads.emplace_back(
      [&should_cancel, i]() {
        // This is just some arbitrary heavy work
        volatile size_t total = 0;
        for (size_t k = 0; k < 549528914167; k++) {
          if (should_cancel) {
            break;
          }
          total += k * (i + 42);
          (void)total;
        }
      });
  }

  // Create an executor
  auto executor = std::make_shared<ExecutorType>();
  // Start spinning
  auto executor_thread = std::thread(
    [executor]() {
      executor->spin();
    });
  // Add a node to the executor
  executor->add_node(this->node);

  // Cancel the executor (make sure that it's already spinning first)
  while (!executor->is_spinning() && rclcpp::ok()) {
    continue;
  }
  executor->cancel();

  // Try to join the thread after cancelling the executor
  // This is the "test". We want to make sure that we can still cancel the executor
  // regardless of the presence of race conditions
  executor_thread.join();

  // The test is now completed: we can join the stress threads
  should_cancel = true;
  for (auto & t : stress_threads) {
    t.join();
  }
}

// Check spin_until_future_complete with node base pointer (instantiates its own executor)
TEST(TestExecutors, testSpinUntilFutureCompleteNodeBasePtr)
{
  rclcpp::init(0, nullptr);

  {
    auto node = std::make_shared<rclcpp::Node>("node");

    std::promise<bool> promise;
    std::future<bool> future = promise.get_future();
    promise.set_value(true);

    auto shared_future = future.share();
    auto ret = rclcpp::spin_until_future_complete(
      node->get_node_base_interface(), shared_future, 1s);
    EXPECT_EQ(rclcpp::FutureReturnCode::SUCCESS, ret);
  }

  rclcpp::shutdown();
}

// Check spin_until_future_complete with node pointer (instantiates its own executor)
TEST(TestExecutors, testSpinUntilFutureCompleteNodePtr)
{
  rclcpp::init(0, nullptr);

  {
    auto node = std::make_shared<rclcpp::Node>("node");

    std::promise<bool> promise;
    std::future<bool> future = promise.get_future();
    promise.set_value(true);

    auto shared_future = future.share();
    auto ret = rclcpp::spin_until_future_complete(node, shared_future, 1s);
    EXPECT_EQ(rclcpp::FutureReturnCode::SUCCESS, ret);
  }

  rclcpp::shutdown();
}

template<typename T>
class TestIntraprocessExecutors : public ::testing::Test
{
public:
  static void SetUpTestCase()
  {
    rclcpp::init(0, nullptr);
  }

  static void TearDownTestCase()
  {
    rclcpp::shutdown();
  }

  void SetUp()
  {
    const auto test_info = ::testing::UnitTest::GetInstance()->current_test_info();
    std::stringstream test_name;
    test_name << test_info->test_case_name() << "_" << test_info->name();
    node = std::make_shared<rclcpp::Node>("node", test_name.str());

    callback_count = 0u;

    const std::string topic_name = std::string("topic_") + test_name.str();

    rclcpp::PublisherOptions po;
    po.use_intra_process_comm = rclcpp::IntraProcessSetting::Enable;
    publisher = node->create_publisher<test_msgs::msg::Empty>(topic_name, rclcpp::QoS(1), po);

    auto callback = [this](test_msgs::msg::Empty::ConstSharedPtr) {
        this->callback_count.fetch_add(1u);
      };

    rclcpp::SubscriptionOptions so;
    so.use_intra_process_comm = rclcpp::IntraProcessSetting::Enable;
    subscription =
      node->create_subscription<test_msgs::msg::Empty>(
      topic_name, rclcpp::QoS(kNumMessages), std::move(callback), so);
  }

  void TearDown()
  {
    publisher.reset();
    subscription.reset();
    node.reset();
  }

  const size_t kNumMessages = 100;

  rclcpp::Node::SharedPtr node;
  rclcpp::Publisher<test_msgs::msg::Empty>::SharedPtr publisher;
  rclcpp::Subscription<test_msgs::msg::Empty>::SharedPtr subscription;
  std::atomic_size_t callback_count;
};

TYPED_TEST_SUITE(TestIntraprocessExecutors, ExecutorTypes, ExecutorTypeNames);

TYPED_TEST(TestIntraprocessExecutors, testIntraprocessRetrigger) {
  // This tests that executors will continue to service intraprocess subscriptions in the case
  // that publishers aren't continuing to publish.
  // This was previously broken in that intraprocess guard conditions were only triggered on
  // publish and the test was added to prevent future regressions.
  static constexpr size_t kNumMessages = 100;

  using ExecutorType = TypeParam;
  ExecutorType executor;
  executor.add_node(this->node);

  EXPECT_EQ(0u, this->callback_count.load());
  this->publisher->publish(test_msgs::msg::Empty());

  // Wait for up to 5 seconds for the first message to come available.
  const std::chrono::milliseconds sleep_per_loop(10);
  int loops = 0;
  while (1u != this->callback_count.load() && loops < 500) {
    rclcpp::sleep_for(sleep_per_loop);
    executor.spin_some();
    loops++;
  }
  EXPECT_EQ(1u, this->callback_count.load());

  // reset counter
  this->callback_count.store(0u);

  for (size_t ii = 0; ii < kNumMessages; ++ii) {
    this->publisher->publish(test_msgs::msg::Empty());
  }

  // Fire a timer every 10ms up to 5 seconds waiting for subscriptions to be read.
  loops = 0;
  auto timer = this->node->create_wall_timer(
    std::chrono::milliseconds(10), [this, &executor, &loops]() {
      loops++;
      if (kNumMessages == this->callback_count.load() || loops == 500) {
        executor.cancel();
      }
    });
  executor.spin();
  EXPECT_EQ(kNumMessages, this->callback_count.load());
}
