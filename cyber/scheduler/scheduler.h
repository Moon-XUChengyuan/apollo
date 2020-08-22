/******************************************************************************
 * Copyright 2018 The Apollo Authors. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *****************************************************************************/

#ifndef CYBER_SCHEDULER_SCHEDULER_H_
#define CYBER_SCHEDULER_SCHEDULER_H_

#include <unistd.h>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "cyber/base/atomic_hash_map.h"
#include "cyber/base/atomic_rw_lock.h"
#include "cyber/common/log.h"
#include "cyber/common/macros.h"
#include "cyber/common/types.h"
#include "cyber/croutine/croutine.h"
#include "cyber/croutine/routine_factory.h"
#include "cyber/proto/choreography_conf.pb.h"
#include "cyber/scheduler/common/mutex_wrapper.h"
#include "cyber/scheduler/common/pin_thread.h"

namespace apollo {
namespace cyber {
namespace scheduler {

using apollo::cyber::base::AtomicHashMap;
using apollo::cyber::base::AtomicRWLock;
using apollo::cyber::base::ReadLockGuard;
using apollo::cyber::croutine::CRoutine;
using apollo::cyber::croutine::RoutineFactory;
using apollo::cyber::data::DataVisitorBase;
using apollo::cyber::proto::InnerThread;//对应调度配置中的thread

class Processor;
class ProcessorContext;

class Scheduler {
 public:
  virtual ~Scheduler() {}
  static Scheduler* Instance();

  bool CreateTask(const RoutineFactory& factory, const std::string& name);//两种创建task的方式：协程工厂，任务体 & data visitor
  bool CreateTask(std::function<void()>&& func, const std::string& name,
                  std::shared_ptr<DataVisitorBase> visitor = nullptr);

  //唤醒task
  bool NotifyTask(uint64_t crid);

  void Shutdown();
  uint32_t TaskPoolSize() { return task_pool_size_; }
  //移除task
  virtual bool RemoveTask(const std::string& name) = 0;
  //线程级别资源控制
  void ProcessLevelResourceControl();
  //对应配置文件中单独的thread
  void SetInnerThreadAttr(const std::string& name, std::thread* thr);
  //分发任务
  virtual bool DispatchTask(const std::shared_ptr<CRoutine>&) = 0;

  //唤醒processor
  virtual bool NotifyProcessor(uint64_t crid) = 0;
  //移除协程
  virtual bool RemoveCRoutine(uint64_t crid) = 0;
  //检查调度状态
  void CheckSchedStatus();
  //设置内部线程配置
  void SetInnerThreadConfs(
      const std::unordered_map<std::string, InnerThread>& confs) {
    inner_thr_confs_ = confs;
  }

 protected:
  Scheduler() : stop_(false) {}

  AtomicRWLock id_cr_lock_;
  AtomicHashMap<uint64_t, MutexWrapper*> id_map_mutex_;
  std::mutex cr_wl_mtx_;

  std::unordered_map<uint64_t, std::shared_ptr<CRoutine>> id_cr_;//协程id-协程 map
  std::vector<std::shared_ptr<ProcessorContext>> pctxs_;//processor context组（vector）
  std::vector<std::shared_ptr<Processor>> processors_;//processor组（vector）

  std::unordered_map<std::string, InnerThread> inner_thr_confs_;//调度配置中单独的线程

  std::string process_level_cpuset_;
  uint32_t proc_num_ = 0;
  uint32_t task_pool_size_ = 0;
  std::atomic<bool> stop_;
};

}  // namespace scheduler
}  // namespace cyber
}  // namespace apollo

#endif  // CYBER_SCHEDULER_SCHEDULER_H_
