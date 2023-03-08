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

/*
1.有一说一这个读写锁的设计还是很牛逼的，以我自己感觉atomic和读写完全不相干的东西竟然可以用前者实现后者
2.这个设计理念是不是百度阿波罗团队自创的呢?好像不是，这有个连接，设计理念很相似，15年的博客：
  https://blog.csdn.net/10km/article/details/49641691
*/

#ifndef CYBER_BASE_ATOMIC_RW_LOCK_H_
#define CYBER_BASE_ATOMIC_RW_LOCK_H_

#include <unistd.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <thread>

#include "cyber/base/rw_lock_guard.h"

namespace apollo {
namespace cyber {
namespace base {

class AtomicRWLock {
  //声明两个友元类，这两个类主要是利用C++的RAII机制对加锁和开锁的封装
  //这两个类主要调用本类的四个privte接口：ReadLock，WriteLock，ReadUnlock，WriteUnlock
  //就像C11中的std::mutex和std::lock_guand之间的关系
  friend class ReadLockGuard<AtomicRWLock>;
  friend class WriteLockGuard<AtomicRWLock>;

 public:
 // RW_LOCK_FREE和WRITE_EXCLUSIVE都是表示锁的状态，RW_LOCK_FREE表示目前没人占用的意思，WRITE_EXCLUSIVE则表示当前锁被一个写的操作占用
        //这个时候你可能会有疑惑，为啥没有“在读”状态呢？在这里在读是和一个正整数表示的，比如1就表示一个线程在读，2就不是两个线程在读
        //这个时候你可能会还有疑惑，人家状态一般都是用枚举表示，或者宏定义，正因为上述读状态是不确定正整数，这个状态用整数表示稳妥
  static const int32_t RW_LOCK_FREE = 0;
  static const int32_t WRITE_EXCLUSIVE = -1;
  static const uint32_t MAX_RETRY_TIMES = 5; //尝试获取锁的时候连续尝试次数，就像自旋锁那样，连续失败MAX_RETRY_TIMES次则会让出线程的执行权
  AtomicRWLock() {}
  explicit AtomicRWLock(bool write_first) : write_first_(write_first) {}

 private:
  // all these function only can used by ReadLockGuard/WriteLockGuard;
  //下面4个接口是给辅助类ReadLockGuard和WriteLockGuard来调用的
  void ReadLock();
  void WriteLock();

  void ReadUnlock();
  void WriteUnlock();

  AtomicRWLock(const AtomicRWLock&) = delete; //删除拷贝函数和辅助函数，没啥说的，常规操作
  AtomicRWLock& operator=(const AtomicRWLock&) = delete;
  std::atomic<uint32_t> write_lock_wait_num_ = {0}; //等待拿到锁的写操作的个数（肯定是非负整数）
  std::atomic<int32_t> lock_num_ = {0};             //锁当前的状态，对标上述提到的RW_LOCK_FREE和WRITE_EXCLUSIVE，说实话这个当时困扰了我好久，状态就状态呗，你起个名字叫num，
                                                    //让人联想到锁的个数，或者说次数，但是你仔细看他的类型是int32_t，这意味着他可能是个负数
                                                    //看源代码咋都联系不到一块儿，几乎到了放弃抵抗，
  bool write_first_ = true;//表示优先干啥。假设现在锁被写操作A占用，此时又来了读操作B，过了极短时间又来了写操作C,B和C现在都想获得锁，一旦A释放了锁，别看B是先来的，要是
                            //设置了write_first_=ture则依然是C优先拿到锁资源
};

//看ReadLock之前建议先看WriteLock，后者比较简单，由浅到深比较容易理解和接受
inline void AtomicRWLock::ReadLock() {
  uint32_t retry_times = 0;
  int32_t lock_num = lock_num_.load();//读取锁状态，这个时候你可能会问WriteLock咋就没读呢？compare_exchange_weak就是包含读的操作
  if (write_first_) {//是否优先写锁舔狗上位机会
    do {
      while (lock_num < RW_LOCK_FREE || write_lock_wait_num_.load() > 0) {
        if (++retry_times == MAX_RETRY_TIMES) {
          // saving cpu
          std::this_thread::yield();
          retry_times = 0;
        }
        lock_num = lock_num_.load();
      }
    } while (!lock_num_.compare_exchange_weak(lock_num, lock_num + 1,
                                              std::memory_order_acq_rel,
                                              std::memory_order_relaxed));
  } else {
    do {
      //仔细比对，write_first_的区别就是下面一行的write_lock_wait_num_.load() > 0判断。翻译为：看看舔狗队列中有没有存在写锁的，有的话自己身为读锁就继续循环，再等等
      while (lock_num < RW_LOCK_FREE) {
        if (++retry_times == MAX_RETRY_TIMES) {
          // saving cpu
          std::this_thread::yield();
          retry_times = 0;
        }
        lock_num = lock_num_.load();
      }
    //程序能走到这里，就是意味着lock_num>=0 且 write_lock_wait_num_==0
    //下面两个情况：
    //1.lock_num_==lock_num(即大于等于0)，可能你会说这不是肯定的吗？这个还真不一定，虽然刚执行过lock_num = lock_num_.load();但是lock_num是个多线程控制的值，随时在变
    //接着说情况1，lock_num_==lock_num  lock_num_赋值加+1 返回true  循环结束
    //     情况2，lock_num_!=lock_num  lock_num赋值为lock_num_（无用） 重新循环 简单点来讲就是：刚刚的判断好好的，正当我要办事的时候，突然有线程偷偷改变了值，一切判断作废，重新来过
    } while (!lock_num_.compare_exchange_weak(lock_num, lock_num + 1,
                                              std::memory_order_acq_rel,
                                              std::memory_order_relaxed));
  }
}

 //看WriteLock之前建议先看ReadUnlock和WriteUnlock，更容易理解和接受lock_num_是干啥的
inline void AtomicRWLock::WriteLock() {
  int32_t rw_lock_free = RW_LOCK_FREE;//用变量获取RW_LOCK_FREE的值，方便给compare_exchange_weak传递一个参数
  uint32_t retry_times = 0;//连续尝试次数
  write_lock_wait_num_.fetch_add(1);//记录等待获取锁权限的写锁个数，为啥专门还记录一下呢？主要为了控制等待获取锁的先后顺序
                                    //简单点来讲，只要有写锁想要获得锁权限（尚未获得），你读锁就往后站站，等我先完活
  //下面这个循环分为下面两个情况：
  //lock_num_==rw_lock_free==0  现在没人用锁  lock_num_变为WRITE_EXCLUSIVE（即-1）  返回值true  循环结束
  //lock_num_!=rw_lock_free 又分为两种情况，情况1：lock_num_==WRITE_EXCLUSIVE  情况2：lock_num_>=1。无论情况1和情况2：
  //                            现在有人用锁   lock_num_不变 rw_lock_free变为lock_num_（但是此时该值无用）  返回false  循环进入
  while (!lock_num_.compare_exchange_weak(rw_lock_free, WRITE_EXCLUSIVE,
                                          std::memory_order_acq_rel,
                                          std::memory_order_relaxed)) {
    // rw_lock_free will change after CAS fail, so init agin
    rw_lock_free = RW_LOCK_FREE;//重置rw_lock_free，因为只要进来，rw_lock_free的值已经被改变，下次循环不好作标志
    if (++retry_times == MAX_RETRY_TIMES) {//判断循环次数，
      // saving cpu
      std::this_thread::yield();//到达指定次数，让出cpu执行权限，等待下次调用机会
      retry_times = 0;
    }
  }
  write_lock_wait_num_.fetch_sub(1);//走到这里意味着自己已经成功拿到了锁权限，而不是只是在等待机会的添狗，舔狗个数减1
}

//上面说过，一旦读锁拿到锁则意味着lock_num_是正整数，也就是+1,当他释放该锁的时候将lock_num_减去1很好理解
inline void AtomicRWLock::ReadUnlock() { lock_num_.fetch_sub(1); }
//上面说过，锁自由状态是0，被写锁占用时候状态是-1，写锁释放的时候加上1也很好理解，就是要从-1转到0嘛
inline void AtomicRWLock::WriteUnlock() { lock_num_.fetch_add(1); }

}  // namespace base
}  // namespace cyber
}  // namespace apollo

#endif  // CYBER_BASE_ATOMIC_RW_LOCK_H_
