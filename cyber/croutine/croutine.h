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

#ifndef CYBER_CROUTINE_CROUTINE_H_
#define CYBER_CROUTINE_CROUTINE_H_

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>

#include "cyber/common/log.h"
#include "cyber/croutine/detail/routine_context.h"

namespace apollo {
namespace cyber {
namespace croutine {

using RoutineFunc = std::function<void()>;//协程要执行的任务
using Duration = std::chrono::microseconds;

enum class RoutineState { READY, FINISHED, SLEEP, IO_WAIT, DATA_WAIT };//协程状态枚举，5种

class CRoutine {
 public:
  explicit CRoutine(const RoutineFunc &func);
  virtual ~CRoutine();

  // static interfaces
  static void Yield();
  static void Yield(const RoutineState &state);
  static void SetMainContext(const std::shared_ptr<RoutineContext> &context);//协程执行环境，栈等
  static CRoutine *GetCurrentRoutine();
  static char **GetMainStack();

  // public interfaces
  bool Acquire();
  void Release();

  // It is caller's responsibility to check if state_ is valid before calling
  // SetUpdateFlag().
  void SetUpdateFlag();

  // acquire && release should be called before Resume
  // when work-steal like mechanism used
  RoutineState Resume();
  RoutineState UpdateState();
  RoutineContext *GetContext();
  char **GetStack();

  void Run();
  void Stop();
  void Wake();
  void HangUp();
  void Sleep(const Duration &sleep_duration);

  // getter and setter
  RoutineState state() const;
  void set_state(const RoutineState &state);

  uint64_t id() const;
  void set_id(uint64_t id);

  const std::string &name() const;
  void set_name(const std::string &name);

  int processor_id() const;
  void set_processor_id(int processor_id);

  uint32_t priority() const;
  void set_priority(uint32_t priority);

  std::chrono::steady_clock::time_point wake_time() const;

  void set_group_name(const std::string &group_name) {
    group_name_ = group_name;
  }

  const std::string &group_name() { return group_name_; }

 private:
  CRoutine(CRoutine &) = delete;
  CRoutine &operator=(CRoutine &) = delete;

  std::string name_;//协程名
  std::chrono::steady_clock::time_point wake_time_ =
      std::chrono::steady_clock::now();//苏醒时间

  RoutineFunc func_;//执行任务
  RoutineState state_;//协程状态

  std::shared_ptr<RoutineContext> context_;//执行环境

  std::atomic_flag lock_ = ATOMIC_FLAG_INIT;//原子标志-锁
  std::atomic_flag updated_ = ATOMIC_FLAG_INIT;//原子标志-被更新
  //原子操作是不可打断的最低粒度操作，是线程安全的
  //原子操作是一种lock free的操作，不需要同步锁，具有很高的性能。
  //原子类提供的成员函数都是原子的，是线程安全的。
  //atomic_flag，只有两种操作：test and set、clear
  //test_and_set保证多线程环境下只被设置一次
  //clear状态可以理解为bool类型的false，set状态可理解为true状态。
  //test_and_set会检测flag是否处于set状态，如果不是，则将其设置为set状态，并返回false；否则返回true。

  bool force_stop_ = false;//被迫停止标志

  int processor_id_ = -1;//绑定process的id
  uint32_t priority_ = 0;//协程优先级
  uint64_t id_ = 0;//协程id

  std::string group_name_;//所在组名

  static thread_local CRoutine *current_routine_;//processor当前协程
  static thread_local char *main_stack_;//主栈
  //C++中变量的4种存储周期（automatic，static，dynamic，thread）
  //thread_local 关键字修饰的变量具有线程（thread）周期，这些变量在线程开始的时候被生成，在线程结束的时候被销毁
  //如果类的成员函数内定义了 thread_local 变量，则对于同一个线程内的该类的多个对象都会共享一个变量实例
};

inline void CRoutine::Yield(const RoutineState &state) {
  auto routine = GetCurrentRoutine();
  routine->set_state(state);
  SwapContext(GetCurrentRoutine()->GetStack(), GetMainStack());
}

inline void CRoutine::Yield() {
  SwapContext(GetCurrentRoutine()->GetStack(), GetMainStack());
}

inline CRoutine *CRoutine::GetCurrentRoutine() { return current_routine_; }

inline char **CRoutine::GetMainStack() { return &main_stack_; }

inline RoutineContext *CRoutine::GetContext() { return context_.get(); }

inline char **CRoutine::GetStack() { return &(context_->sp); }

inline void CRoutine::Run() { func_(); }//执行任务

inline void CRoutine::set_state(const RoutineState &state) { state_ = state; }

inline RoutineState CRoutine::state() const { return state_; }

inline std::chrono::steady_clock::time_point CRoutine::wake_time() const {
  return wake_time_;
}

inline void CRoutine::Wake() { state_ = RoutineState::READY; }//苏醒，状态置为ready

inline void CRoutine::HangUp() { CRoutine::Yield(RoutineState::DATA_WAIT); }//挂起，状态置为data_wait

inline void CRoutine::Sleep(const Duration &sleep_duration) {//睡眠，设置苏醒时间，状态置为sleep
  wake_time_ = std::chrono::steady_clock::now() + sleep_duration;
  CRoutine::Yield(RoutineState::SLEEP);
}

inline uint64_t CRoutine::id() const { return id_; }

inline void CRoutine::set_id(uint64_t id) { id_ = id; }

inline const std::string &CRoutine::name() const { return name_; }

inline void CRoutine::set_name(const std::string &name) { name_ = name; }

inline int CRoutine::processor_id() const { return processor_id_; }

inline void CRoutine::set_processor_id(int processor_id) {
  processor_id_ = processor_id;
}

inline RoutineState CRoutine::UpdateState() {//更新状态，并返回新的状态
  // Synchronous Event Mechanism
  //sleep，睡眠时间到则置为ready
  if (state_ == RoutineState::SLEEP &&
      std::chrono::steady_clock::now() > wake_time_) {
    state_ = RoutineState::READY;
    return state_;
  }

  // Asynchronous Event Mechanism
  if (!updated_.test_and_set(std::memory_order_release)) {//检查updated标志（限制cpu指令重排）
    //wait，状态置为ready
    if (state_ == RoutineState::DATA_WAIT || state_ == RoutineState::IO_WAIT) {
      state_ = RoutineState::READY;
    }
  }
  return state_;
}

inline uint32_t CRoutine::priority() const { return priority_; }

inline void CRoutine::set_priority(uint32_t priority) { priority_ = priority; }

inline bool CRoutine::Acquire() {//获取
  return !lock_.test_and_set(std::memory_order_acquire);
}

inline void CRoutine::Release() {//释放
  return lock_.clear(std::memory_order_release);
}

inline void CRoutine::SetUpdateFlag() {//clear updated标志
  updated_.clear(std::memory_order_release);
}

}  // namespace croutine
}  // namespace cyber
}  // namespace apollo

#endif  // CYBER_CROUTINE_CROUTINE_H_
