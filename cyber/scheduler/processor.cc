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

#include "cyber/scheduler/processor.h"

#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <chrono>

#include "cyber/common/global_data.h"
#include "cyber/common/log.h"
#include "cyber/croutine/croutine.h"
#include "cyber/time/time.h"

namespace apollo {
namespace cyber {
namespace scheduler {

using apollo::cyber::common::GlobalData;

Processor::Processor() { running_.store(true); }

Processor::~Processor() { Stop(); }

void Processor::Run() {
  tid_.store(static_cast<int>(syscall(SYS_gettid)));//通过Linux系统调用获得thread唯一tid
  AINFO << "processor_tid: " << tid_;
  snap_shot_->processor_id.store(tid_);

  //process循环工作
  while (cyber_likely(running_.load())) {
    //已经绑定了context_
    if (cyber_likely(context_ != nullptr)) {
      auto croutine = context_->NextRoutine();//获取下一个协程（取决于context类型）
      if (croutine) {//获取到协程则记录快照并执行
        snap_shot_->execute_start_time.store(cyber::Time::Now().ToNanosecond());
        snap_shot_->routine_name = croutine->name();
        croutine->Resume();
        croutine->Release();
      } else {//未获取到协程则context_进行等待
        snap_shot_->execute_start_time.store(0);
        context_->Wait();
      }
    } 
    //还未绑定context_
    else {
      std::unique_lock<std::mutex> lk(mtx_ctx_);
      cv_ctx_.wait_for(lk, std::chrono::milliseconds(10));//等待绑定context_
    }
  }
}

void Processor::Stop() {
  //processor停止工作
  if (!running_.exchange(false)) {//状态切换
    return;
  }

  if (context_) {//context_停止
    context_->Shutdown();
  }

  cv_ctx_.notify_one();//唤醒一个等待线程
  if (thread_.joinable()) {
    thread_.join();
  }
}

void Processor::BindContext(const std::shared_ptr<ProcessorContext>& context) {
  //绑定context_
  context_ = context;
  std::call_once(thread_flag_,
                 [this]() { thread_ = std::thread(&Processor::Run, this); });//单例模式
}

std::atomic<pid_t>& Processor::Tid() {
  while (tid_.load() == -1) {
    cpu_relax();//内存屏障；让cpu松弛下来降低功耗，把资源配置给其他thread等
  }
  return tid_;
}

}  // namespace scheduler
}  // namespace cyber
}  // namespace apollo
