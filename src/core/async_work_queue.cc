// Copyright (c) 2020, NVIDIA CORPORATION. All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
//  * Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
//  * Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
//  * Neither the name of NVIDIA CORPORATION nor the names of its
//    contributors may be used to endorse or promote products derived
//    from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
// PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
// CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
// PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
// OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "src/core/async_work_queue.h"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace nvidia { namespace inferenceserver {

AsyncWorkQueue::~AsyncWorkQueue()
{
  for (size_t cnt = 0; cnt < worker_threads_.size(); cnt++) {
    GetSingleton()->task_queue_.Put(nullptr);
  }
  for (const auto& worker_thread : worker_threads_) {
    worker_thread->join();
  }
}

AsyncWorkQueue*
AsyncWorkQueue::GetSingleton()
{
  static AsyncWorkQueue singleton;
  return &singleton;
}

Status
AsyncWorkQueue::Initialize(size_t worker_count)
{
  if (worker_count < 1) {
    return Status(
        Status::Code::INVALID_ARG,
        "Async work queue must be initialized with positive 'worker_count'");
  }
  static std::mutex init_mtx;
  std::lock_guard<std::mutex> lk(init_mtx);
  if (GetSingleton()->worker_threads_.size() == 0) {
    for (size_t cnt = 0; cnt < worker_count; cnt++) {
      GetSingleton()->worker_threads_.push_back(
          std::unique_ptr<std::thread>(new std::thread([] { WorkThread(); })));
    }
  }
  return Status::Success;
}

size_t
AsyncWorkQueue::WorkerCount()
{
  return GetSingleton()->worker_threads_.size();
}

Status
AsyncWorkQueue::AddTask(const std::function<void(void)>&& task)
{
  if (GetSingleton()->worker_threads_.size() == 0) {
    return Status(
        Status::Code::UNAVAILABLE,
        "Async work queue must be initialized before adding task");
  }
  GetSingleton()->task_queue_.Put(std::move(task));
  return Status::Success;
}

Status
AsyncWorkQueue::AddBundledTask(const std::function<void(size_t)>&& task)
{
  auto singleton = GetSingleton();
  if (singleton->worker_threads_.size() == 0) {
    return Status(
        Status::Code::UNAVAILABLE,
        "Async work queue must be initialized before adding task");
  }
  {
    std::lock_guard<std::mutex> lk(singleton->mtx_);
    singleton->bundled_task_queue_.push_back(std::move(task));
  }
  if (singleton->task_queue_.Empty()) {
    singleton->SplitBundledTasks();
  }
  return Status::Success;
}

void
AsyncWorkQueue::SplitBundledTasks()
{
  std::deque<std::function<void(size_t)>> local_bundled_task_queue;
  {
    std::lock_guard<std::mutex> lk(mtx_);
    // Only swap the queue if it is not empty, otherwise, other thread entered
    // this function concurrently and take the job of splitting the tasks
    if (!bundled_task_queue_.empty()) {
      bundled_task_queue_.swap(local_bundled_task_queue);
    }
  }
  if (!local_bundled_task_queue.empty()) {
    size_t thread_per_task = std::max((size_t)1, worker_threads_.size() / local_bundled_task_queue.size());
    while (!local_bundled_task_queue.empty()) {
      local_bundled_task_queue.front()(thread_per_task);
      local_bundled_task_queue.pop_front();
    }
  }
}

void
AsyncWorkQueue::WorkThread()
{
  while (true) {
    if (GetSingleton()->task_queue_.Empty()) {
      GetSingleton()->SplitBundledTasks();
    }
    auto task = GetSingleton()->task_queue_.Get();
    if (task != nullptr) {
      task();
    } else {
      break;
    }
  }
}

}}  // namespace nvidia::inferenceserver
