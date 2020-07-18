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

#ifndef CYBER_SCHEDULER_PROCESSOR_H_
#define CYBER_SCHEDULER_PROCESSOR_H_

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "cyber/croutine/croutine.h"
#include "cyber/proto/scheduler_conf.pb.h"
#include "cyber/scheduler/processor_context.h"

namespace apollo {
namespace cyber {
namespace scheduler {

using croutine::CRoutine;

//结构体-快照：执行开始时间，process_id，协程名
struct Snapshot {
  std::atomic<uint64_t> execute_start_time = {0};
  std::atomic<pid_t> processor_id = {0};
  std::string routine_name;
};

class Processor {
 public:
  Processor();
  virtual ~Processor();

  void Run();
  void Stop();
  void BindContext(const std::shared_ptr<ProcessorContext>& context);
  std::thread* Thread() { return &thread_; }
  std::atomic<pid_t>& Tid();

  std::shared_ptr<Snapshot> ProcSnapshot() { return snap_shot_; }

 private:
  std::shared_ptr<ProcessorContext> context_;//process绑定的上下文

  std::condition_variable cv_ctx_;//条件变量
  std::once_flag thread_flag_;//单例模式辅助变量
  std::mutex mtx_ctx_;//互斥锁
  std::thread thread_;//process对应的thread

  std::atomic<pid_t> tid_{-1};//原子类型：thread对应的tid
  std::atomic<bool> running_{false};//原子类型，针对原子类型的操作要么完成要么不做

  std::shared_ptr<Snapshot> snap_shot_ = std::make_shared<Snapshot>();
};

}  // namespace scheduler
}  // namespace cyber
}  // namespace apollo

#endif  // CYBER_SCHEDULER_PROCESSOR_H_
