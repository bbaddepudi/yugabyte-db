// Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//

#include <atomic>
#include <future>
#include <mutex>
#include <random>
#include <stack>
#include <thread>

#include "yb/docdb/shared_lock_manager.h"

#include "yb/rpc/thread_pool.h"

#include "yb/util/random_util.h"
#include "yb/util/test_macros.h"
#include "yb/util/test_util.h"

using namespace std::literals;

using std::string;
using std::vector;
using std::stack;
using std::thread;

namespace yb {
namespace docdb {

const RefCntPrefix kKey1("foo");
const RefCntPrefix kKey2("bar");

class SharedLockManagerTest : public YBTest {
 protected:
  SharedLockManagerTest();

 protected:
  SharedLockManager lm_;

  LockBatch TestLockBatch() {
    return LockBatch(&lm_, {
        {kKey1, IntentTypeSet({IntentType::kStrongWrite, IntentType::kStrongRead})},
        {kKey2, IntentTypeSet({IntentType::kStrongWrite, IntentType::kStrongRead})}});
  }
};

SharedLockManagerTest::SharedLockManagerTest() {
}

TEST_F(SharedLockManagerTest, LockBatchAutoUnlockTest) {
  for (int i = 0; i < 2; ++i) {
    auto lb = TestLockBatch();
    EXPECT_EQ(2, lb.size());
    EXPECT_FALSE(lb.empty());
    // The locks get unlocked on scope exit.
  }
}

TEST_F(SharedLockManagerTest, LockBatchMoveConstructor) {
  LockBatch lb = TestLockBatch();
  EXPECT_EQ(2, lb.size());
  EXPECT_FALSE(lb.empty());

  LockBatch lb2(std::move(lb));
  EXPECT_EQ(2, lb2.size());
  EXPECT_FALSE(lb2.empty());

  // lb has been moved from and is now empty
  EXPECT_EQ(0, lb.size());
  EXPECT_TRUE(lb.empty());
}

TEST_F(SharedLockManagerTest, LockBatchMoveAssignment) {
  LockBatch lb = TestLockBatch();

  LockBatch lb2 = std::move(lb);
  EXPECT_EQ(2, lb2.size());
  EXPECT_FALSE(lb2.empty());

  // lb has been moved from and is now empty
  EXPECT_EQ(0, lb.size());
  EXPECT_TRUE(lb.empty());
}

TEST_F(SharedLockManagerTest, LockBatchReset) {
  LockBatch lb = TestLockBatch();
  lb.Reset();

  EXPECT_EQ(0, lb.size());
  EXPECT_TRUE(lb.empty());
}

// Launch pairs of threads. Each pair tries to lock/unlock on the same key sequence.
// This catches bug in SharedLockManager when condition is waited incorrectly.
TEST_F(SharedLockManagerTest, QuickLockUnlock) {
  const auto kThreads = 2 * 32; // Should be even

  std::atomic<bool> stop_requested{false};
  std::vector<std::thread> threads;
  std::atomic<size_t> finished_threads{0};
  while (threads.size() != kThreads) {
    size_t pair_idx = threads.size() / 2;
    threads.emplace_back([this, &stop_requested, &finished_threads, pair_idx] {
      int i = 0;
      while (!stop_requested.load(std::memory_order_acquire)) {
        RefCntPrefix key(Format("key_$0_$1", pair_idx, i));
        LockBatch lb(&lm_,
                     {{key, IntentTypeSet({IntentType::kStrongWrite, IntentType::kStrongRead})}});
        ++i;
      }
      finished_threads.fetch_add(1, std::memory_order_acq_rel);
    });
  }

  std::this_thread::sleep_for(30s);
  LOG(INFO) << "Requesting stop";
  stop_requested.store(true, std::memory_order_release);

  ASSERT_OK(WaitFor(
      [&finished_threads] {
        return finished_threads.load(std::memory_order_acquire) == kThreads;
      },
      3s,
      "All threads finished"));

  for (auto& thread : threads) {
    thread.join();
  }
}

TEST_F(SharedLockManagerTest, LockConflicts) {
  rpc::ThreadPool tp(rpc::ThreadPoolOptions{"test_pool"s, 10, 1});

  struct ThreadPoolTask : public rpc::ThreadPoolTask {
   public:
    explicit ThreadPoolTask(SharedLockManager* lock_manager, IntentTypeSet set)
        : lock_manager_(lock_manager), set_(set) {
    }

    virtual ~ThreadPoolTask() {}

    void Run() override {
      LockBatch lb(lock_manager_, {{kKey1, set_}});
    }

    void Done(const Status& status) override {
      promise_.set_value(true);
    }

    std::future<bool> GetFuture() {
      return promise_.get_future();
    }

   private:
    SharedLockManager* lock_manager_;
    IntentTypeSet set_;
    std::promise<bool> promise_;
  };

  for (size_t idx1 = 0; idx1 != kIntentTypeSetMapSize; ++idx1) {
    IntentTypeSet set1(idx1);
    SCOPED_TRACE(Format("Set1: $0", set1));
    for (size_t idx2 = 0; idx2 != kIntentTypeSetMapSize; ++idx2) {
      IntentTypeSet set2(idx2);
      SCOPED_TRACE(Format("Set2: $0", set2));
      LockBatch lb(&lm_, {{kKey1, set1}});
      ThreadPoolTask task(&lm_, set2);
      auto future = task.GetFuture();
      tp.Enqueue(&task);
      if (future.wait_for(200ms) == std::future_status::ready) {
        // Lock on set2 was taken fast enough, it means that sets should NOT conflict.
        ASSERT_FALSE(IntentTypeSetsConflict(set1, set2));
      } else {
        // Lock on set2 was taken not taken for too long, it means that sets should conflict.
        ASSERT_TRUE(IntentTypeSetsConflict(set1, set2));
        // Release lock on set1 and check that set2 was successfully locked after it.
        lb.Reset();
        ASSERT_EQ(future.wait_for(200ms), std::future_status::ready);
      }
    }
  }

  tp.Shutdown();
}

} // namespace docdb
} // namespace yb
