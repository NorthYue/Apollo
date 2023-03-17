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

#include "cyber/timer/timer.h"

#include <cmath>

#include "cyber/common/global_data.h"

namespace apollo {
namespace cyber {

namespace {
std::atomic<uint64_t> global_timer_id = {0};
uint64_t GenerateTimerId() { return global_timer_id.fetch_add(1); }
}  // namespace

Timer::Timer() {
  timing_wheel_ = TimingWheel::Instance();
  timer_id_ = GenerateTimerId();
}

Timer::Timer(TimerOption opt) : timer_opt_(opt) {
  timing_wheel_ = TimingWheel::Instance();
  timer_id_ = GenerateTimerId();
}

Timer::Timer(uint32_t period, std::function<void()> callback, bool oneshot) {
  timing_wheel_ = TimingWheel::Instance();
  timer_id_ = GenerateTimerId();
  timer_opt_.period = period;
  timer_opt_.callback = callback;
  timer_opt_.oneshot = oneshot;
}

void Timer::SetTimerOption(TimerOption opt) { timer_opt_ = opt; }

bool Timer::InitTimerTask() {
  if (timer_opt_.period == 0) {
    AERROR << "Max interval must great than 0";
    return false;
  }

  if (timer_opt_.period >= TIMER_MAX_INTERVAL_MS) {
    AERROR << "Max interval must less than " << TIMER_MAX_INTERVAL_MS;
    return false;
  }

  // 1. 初始化定时任务
  task_.reset(new TimerTask(timer_id_));
  task_->interval_ms = timer_opt_.period;
  task_->next_fire_duration_ms = task_->interval_ms;
  // 2. 是否单次触发
  if (timer_opt_.oneshot) {
    std::weak_ptr<TimerTask> task_weak_ptr = task_;
    // 3. 注册任务回调
    task_->callback = [callback = this->timer_opt_.callback, task_weak_ptr]() {
      auto task = task_weak_ptr.lock();
      if (task) {
        std::lock_guard<std::mutex> lg(task->mutex);
        callback();
      }
    };
  } else {
    std::weak_ptr<TimerTask> task_weak_ptr = task_;
    task_->callback = [callback = this->timer_opt_.callback, task_weak_ptr]() {
      auto task = task_weak_ptr.lock();
      if (!task) {
        return;
      }
      std::lock_guard<std::mutex> lg(task->mutex);
      auto start = Time::MonoTime().ToNanosecond();
      callback();
      auto end = Time::MonoTime().ToNanosecond();
      uint64_t execute_time_ns = end - start;
      uint64_t execute_time_ms =
#if defined(__aarch64__)
          ::llround(static_cast<double>(execute_time_ns) / 1e6);
#else
          std::llround(static_cast<double>(execute_time_ns) / 1e6);
#endif
      if (task->last_execute_time_ns == 0) {
        task->last_execute_time_ns = start;
      } else {
        // 累积时间误差
        // start - task->last_execute_time_ns 为2次执行真实间隔时间，task->interval_ms是设定的间隔时间
        // 注意误差会修复补偿（每次插入任务时候会修复），因此这里用的是累计，2次误差会抵消，保持绝对误差为0
        task->accumulated_error_ns +=
            start - task->last_execute_time_ns - task->interval_ms * 1000000;
      }
      ADEBUG << "start: " << start << "\t last: " << task->last_execute_time_ns
             << "\t execut time:" << execute_time_ms
             << "\t accumulated_error_ns: " << task->accumulated_error_ns;
      task->last_execute_time_ns = start;
      // 如果执行时间大于任务周期时间，则下一个tick马上执行
      if (execute_time_ms >= task->interval_ms) {
        task->next_fire_duration_ms = TIMER_RESOLUTION_MS;
      } else {
#if defined(__aarch64__)
        int64_t accumulated_error_ms = ::llround(
#else
        int64_t accumulated_error_ms = std::llround(
#endif
            static_cast<double>(task->accumulated_error_ns) / 1e6);
        if (static_cast<int64_t>(task->interval_ms - execute_time_ms -
                                 TIMER_RESOLUTION_MS) >= accumulated_error_ms) {
          // 这里会补偿误差
          // task->next_fire_duration_ms是任务下一次执行的间隔，这个间隔是以task执行完成之后为起始时间的，
          // 因为每次插入新任务到时间轮都是在用户"callback"函数执行之后进行的，因此这里的时间起点也是以这个时间为准。
          task->next_fire_duration_ms =
              task->interval_ms - execute_time_ms - accumulated_error_ms;
        } else {
          task->next_fire_duration_ms = TIMER_RESOLUTION_MS;
        }
        ADEBUG << "error ms: " << accumulated_error_ms
               << "  execute time: " << execute_time_ms
               << " next fire: " << task->next_fire_duration_ms
               << " error ns: " << task->accumulated_error_ns;
      }
      // 将新的任务放到下一个时间轮bucket中
      TimingWheel::Instance()->AddTask(task);
    };
  }
  return true;
}

void Timer::Start() {
  if (!common::GlobalData::Instance()->IsRealityMode()) {
    return;
  }

  // 1. 首先判断定时器是否已经启动
  if (!started_.exchange(true)) {
    // 2. 初始化任务
    if (InitTimerTask()) {
      // 3. 在时间轮中增加任务
      timing_wheel_->AddTask(task_);
      AINFO << "start timer [" << task_->timer_id_ << "]";
    }
  }
}

void Timer::Stop() {
  if (started_.exchange(false) && task_) {
    AINFO << "stop timer, the timer_id: " << timer_id_;
    // using a shared pointer to hold task_->mutex before task_ reset
    auto tmp_task = task_;
    {
      std::lock_guard<std::mutex> lg(tmp_task->mutex);
      task_.reset();
    }
  }
}

Timer::~Timer() {
  if (task_) {
    Stop();
  }
}

}  // namespace cyber
}  // namespace apollo
