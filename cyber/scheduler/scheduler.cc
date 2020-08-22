/******************************************************************************
 * Copyright 2018 The Apollo Authors. All Rights Reserved.
 *
 * Licensed under the Apache License, Vesched_infoon 2.0 (the "License");
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

#include "cyber/scheduler/scheduler.h"

#include <sched.h>
#include <utility>

#include "cyber/common/environment.h"
#include "cyber/common/file.h"
#include "cyber/common/global_data.h"
#include "cyber/common/util.h"
#include "cyber/data/data_visitor.h"
#include "cyber/scheduler/processor.h"
#include "cyber/scheduler/processor_context.h"

namespace apollo {
namespace cyber {
namespace scheduler {

using apollo::cyber::common::GlobalData;

bool Scheduler::CreateTask(const RoutineFactory& factory,
                           const std::string& name) {
  return CreateTask(factory.create_routine(), name, factory.GetDataVisitor());//调用重载
}

bool Scheduler::CreateTask(std::function<void()>&& func,
                           const std::string& name,
                           std::shared_ptr<DataVisitorBase> visitor) {
  if (cyber_unlikely(stop_.load())) {//如果调度器已停止
    ADEBUG << "scheduler is stoped, cannot create task!";
    return false;
  }

  auto task_id = GlobalData::RegisterTaskName(name);//给出该task的id

  auto cr = std::make_shared<CRoutine>(func);//生成协程来执行该task，并设置其id与name
  cr->set_id(task_id);
  cr->set_name(name);
  AINFO << "create croutine: " << name;

  if (!DispatchTask(cr)) {//派发该协程，如果不成功则返回false
    return false;
  }

  if (visitor != nullptr) {//派发该协程成功，则返回true
    visitor->RegisterNotifyCallback([this, task_id]() {//注册数据（对应channel）的回调函数为本调度器唤醒本协程对应的processor
      if (cyber_unlikely(stop_.load())) {
        return;
      }
      this->NotifyProcessor(task_id);
    });
  }
  return true;
}

bool Scheduler::NotifyTask(uint64_t crid) {//唤醒协程
  if (cyber_unlikely(stop_.load())) {
    return true;
  }
  return NotifyProcessor(crid);
}

void Scheduler::ProcessLevelResourceControl() {
  std::vector<int> cpus;
  ParseCpuset(process_level_cpuset_, &cpus);//从配置文件解析相应字符串，获取cpus
  cpu_set_t set;
  CPU_ZERO(&set);
  for (const auto cpu : cpus) {
    CPU_SET(cpu, &set);
  }
  pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
  //pthread_self返回的是同一个进程中各个线程之间的标识号，对于这个进程内是唯一的，而不同进程中，每个线程返回的pthread_self可能返回的是一样的
}

void Scheduler::SetInnerThreadAttr(const std::string& name, std::thread* thr) {//设置配置文件中的thread
  if (thr != nullptr && inner_thr_confs_.find(name) != inner_thr_confs_.end()) {
    auto th_conf = inner_thr_confs_[name];
    auto cpuset = th_conf.cpuset();

    std::vector<int> cpus;
    ParseCpuset(cpuset, &cpus);
    SetSchedAffinity(thr, cpus, "range");//设置cpu亲和性，且只能为range
    SetSchedPolicy(thr, th_conf.policy(), th_conf.prio());//设置调度策略及优先级
  }
}

void Scheduler::CheckSchedStatus() {//检查调度状态（调度器快照）
  std::string snap_info;
  auto now = Time::Now().ToNanosecond();
  for (auto processor : processors_) {//对于每一个processor，获取并记录其快照
    auto snap = processor->ProcSnapshot();
    if (snap->execute_start_time.load()) {//若有协程正在执行，则记录 —— processor_id : routine_name : execute_time : timestamp : {now}
      auto execute_time = (now - snap->execute_start_time.load()) / 1000000;
      snap_info.append(std::to_string(snap->processor_id.load()))
          .append(":")
          .append(snap->routine_name)
          .append(":")
          .append(std::to_string(execute_time));
    } else {//若无协程正在执行，则记录 —— processor_id : idle : timestamp : {now}
      snap_info.append(std::to_string(snap->processor_id.load()))
          .append(":idle");
    }
    snap_info.append(", ");
  }
  snap_info.append("timestamp: ").append(std::to_string(now));
  AINFO << snap_info;
  snap_info.clear();
}

void Scheduler::Shutdown() {
  if (cyber_unlikely(stop_.exchange(true))) {//调度器状态切换
    return;
  }

  for (auto& ctx : pctxs_) {//processor上下文停止
    ctx->Shutdown();
  }

  std::vector<uint64_t> cr_list;
  {
    ReadLockGuard<AtomicRWLock> lk(id_cr_lock_);
    for (auto& cr : id_cr_) {
      cr_list.emplace_back(cr.second->id());
    }
  }

  for (auto& id : cr_list) {//移除协程
    RemoveCRoutine(id);
  }

  for (auto& processor : processors_) {//processor停止
    processor->Stop();
  }

  processors_.clear();//清除processor及其上下文
  pctxs_.clear();
}
}  // namespace scheduler
}  // namespace cyber
}  // namespace apollo
