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

#include "cyber/scheduler/policy/classic_context.h"

#include <climits>

namespace apollo {
namespace cyber {
namespace scheduler {

using apollo::cyber::base::AtomicRWLock;
using apollo::cyber::base::ReadLockGuard;
using apollo::cyber::base::WriteLockGuard;
using apollo::cyber::croutine::CRoutine;
using apollo::cyber::croutine::RoutineState;

alignas(CACHELINE_SIZE) GRP_WQ_MUTEX ClassicContext::mtx_wq_;
alignas(CACHELINE_SIZE) GRP_WQ_CV ClassicContext::cv_wq_;
alignas(CACHELINE_SIZE) RQ_LOCK_GROUP ClassicContext::rq_locks_;
alignas(CACHELINE_SIZE) CR_GROUP ClassicContext::cr_group_;
alignas(CACHELINE_SIZE) NOTIFY_GRP ClassicContext::notify_grp_;

ClassicContext::ClassicContext() { InitGroup(DEFAULT_GROUP_NAME); }

ClassicContext::ClassicContext(const std::string& group_name) {
  InitGroup(group_name);
}

void ClassicContext::InitGroup(const std::string& group_name) {
  //初始化数据结构指针及变量
  multi_pri_rq_ = &cr_group_[group_name];
  lq_ = &rq_locks_[group_name];
  mtx_wrapper_ = &mtx_wq_[group_name];
  cw_ = &cv_wq_[group_name];
  notify_grp_[group_name] = 0;
  current_grp = group_name;
}

std::shared_ptr<CRoutine> ClassicContext::NextRoutine() {
  if (cyber_unlikely(stop_.load())) {
    return nullptr;
  }
  
  //从最高优先级队列开始遍历
  for (int i = MAX_PRIO - 1; i >= 0; --i) {
    ReadLockGuard<AtomicRWLock> lk(lq_->at(i));//获取多优先级队列的第i个队列锁
    for (auto& cr : multi_pri_rq_->at(i)) {//遍历地第i个队列中的协程
      if (!cr->Acquire()) {//如果获取协程失败则遍历下一个
        continue;
      }

      if (cr->UpdateState() == RoutineState::READY) {//获取到了协程且状态为ready，则返回
        return cr;
      }

      cr->Release();//获取到协程不为ready则释放，并继续遍历下一个
    }
  }

  return nullptr;//无可执行协程则返回空
}

void ClassicContext::Wait() {
  std::unique_lock<std::mutex> lk(mtx_wrapper_->Mutex());//协程组-互斥量组
  cw_->Cv().wait_for(lk, std::chrono::milliseconds(1000),
                     [&]() { return notify_grp_[current_grp] > 0; });//协程组-信号量组
  if (notify_grp_[current_grp] > 0) {
    notify_grp_[current_grp]--;
  }
}

void ClassicContext::Shutdown() {
  stop_.store(true);
  mtx_wrapper_->Mutex().lock();
  notify_grp_[current_grp] = UCHAR_MAX;
  mtx_wrapper_->Mutex().unlock();
  cw_->Cv().notify_all();//唤醒所有等待线程
}

void ClassicContext::Notify(const std::string& group_name) {
  (&mtx_wq_[group_name])->Mutex().lock();
  notify_grp_[group_name]++;
  (&mtx_wq_[group_name])->Mutex().unlock();
  cv_wq_[group_name].Cv().notify_one();//唤醒一个等待线程
}

bool ClassicContext::RemoveCRoutine(const std::shared_ptr<CRoutine>& cr) {
  auto grp = cr->group_name();
  auto prio = cr->priority();
  auto crid = cr->id();
  WriteLockGuard<AtomicRWLock> lk(ClassicContext::rq_locks_[grp].at(prio));//获取多优先级协程队列中对应优先级队列的锁
  auto& croutines = ClassicContext::cr_group_[grp].at(prio);//获取协程队列
  for (auto it = croutines.begin(); it != croutines.end(); ++it) {//遍历队列
    if ((*it)->id() == crid) {
      auto cr = *it;
      cr->Stop();
      while (!cr->Acquire()) {
        std::this_thread::sleep_for(std::chrono::microseconds(1));
        AINFO_EVERY(1000) << "waiting for task " << cr->name() << " completion";
      }
      croutines.erase(it);//vector容器删除元素
      cr->Release();
      return true;
    }
  }
  return false;
}

}  // namespace scheduler
}  // namespace cyber
}  // namespace apollo
