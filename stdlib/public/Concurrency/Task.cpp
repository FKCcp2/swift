//===--- Task.cpp - Task object and management ----------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// Object management routines for asynchronous task objects.
//
//===----------------------------------------------------------------------===//

#include "swift/Runtime/Concurrency.h"
#include "swift/ABI/Task.h"
#include "swift/ABI/Metadata.h"
#include "swift/Runtime/HeapObject.h"
#include "TaskPrivate.h"

using namespace swift;
using FutureFragment = AsyncTask::FutureFragment;

void FutureFragment::destroy() {
  auto queueHead = waitQueue.load(std::memory_order_acquire);
  switch (queueHead.getStatus()) {
  case Status::Executing:
    assert(false && "destroying a task that never completed");

  case Status::Success:
    resultType->vw_destroy(getStoragePtr());
    break;

  case Status::Error:
    swift_unknownObjectRelease(reinterpret_cast<OpaqueValue *>(getError()));
    break;
  }
}

FutureFragment::Status AsyncTask::waitFuture(AsyncTask *waitingTask) {
  using Status = FutureFragment::Status;
  using WaitQueueItem = FutureFragment::WaitQueueItem;

  assert(isFuture());
  auto fragment = futureFragment();

  auto queueHead = fragment->waitQueue.load(std::memory_order_acquire);
  while (true) {
    switch (queueHead.getStatus()) {
    case Status::Error:
    case Status::Success:
      // The task is done; we don't need to wait.
      return queueHead.getStatus();

    case Status::Executing:
      // Task is now complete. We'll need to add ourselves to the queue.
      break;
    }

    // Put the waiting task at the beginning of the wait queue.
    waitingTask->getNextWaitingTask() = queueHead.getTask();
    auto newQueueHead = WaitQueueItem::get(Status::Executing, waitingTask);
    if (fragment->waitQueue.compare_exchange_weak(
            queueHead, newQueueHead, std::memory_order_release,
            std::memory_order_acquire)) {
      // Escalate the priority of this task based on the priority
      // of the waiting task.
      swift_task_escalate(this, waitingTask->Flags.getPriority());
      return FutureFragment::Status::Executing;
    }
  }
}

void AsyncTask::completeFuture(AsyncContext *context, ExecutorRef executor) {
  using Status = FutureFragment::Status;
  using WaitQueueItem = FutureFragment::WaitQueueItem;

  assert(isFuture());
  auto fragment = futureFragment();

  // If an error was thrown, save it in the future fragment.
  auto futureContext = static_cast<FutureAsyncContext *>(context);
  bool hadErrorResult = false;
  if (auto errorObject = futureContext->errorResult) {
    fragment->getError() = errorObject;
    hadErrorResult = true;
  }

  // Update the status to signal completion.
  auto newQueueHead = WaitQueueItem::get(
    hadErrorResult ? Status::Error : Status::Success,
    nullptr
  );
  auto queueHead = fragment->waitQueue.exchange(
      newQueueHead, std::memory_order_acquire);
  assert(queueHead.getStatus() == Status::Executing);

  // Schedule every waiting task on the executor.
  auto waitingTask = queueHead.getTask();
  while (waitingTask) {
    // Find the next waiting task.
    auto nextWaitingTask = waitingTask->getNextWaitingTask();

    // TODO: schedule this task on the executor rather than running it
    // directly.
    waitingTask->run(executor);

    // Move to the next task.
    waitingTask = nextWaitingTask;
  }
}

SWIFT_CC(swift)
static void destroyTask(SWIFT_CONTEXT HeapObject *obj) {
  auto task = static_cast<AsyncTask*>(obj);

  // For a future, destroy the result.
  if (task->isFuture()) {
    task->futureFragment()->destroy();
  }

  // The task execution itself should always hold a reference to it, so
  // if we get here, we know the task has finished running, which means
  // swift_task_complete should have been run, which will have torn down
  // the task-local allocator.  There's actually nothing else to clean up
  // here.

  free(task);
}

/// Heap metadata for an asynchronous task.
static FullMetadata<HeapMetadata> taskHeapMetadata = {
  {
    {
      &destroyTask
    },
    {
      /*value witness table*/ nullptr
    }
  },
  {
    MetadataKind::Task
  }
};

/// The function that we put in the context of a simple task
/// to handle the final return.
SWIFT_CC(swift)
static void completeTask(AsyncTask *task, ExecutorRef executor,
                         AsyncContext *context) {
  // Tear down the task-local allocator immediately; there's no need
  // to wait for the object to be destroyed.
  _swift_task_alloc_destroy(task);

  // Complete the future.
  if (task->isFuture()) {
    task->completeFuture(context, executor);
  }

  // TODO: set something in the status?
  // TODO: notify the parent somehow?
  // TODO: remove this task from the child-task chain?

  // Release the task, balancing the retain that a running task
  // has on itself.
  swift_release(task);
}

AsyncTaskAndContext
swift::swift_task_create(JobFlags flags, AsyncTask *parent,
                         const AsyncFunctionPointer<void()> *function) {
  return swift_task_create_f(flags, parent, function->Function.get(),
                             function->ExpectedContextSize);
}

AsyncTaskAndContext
swift::swift_task_create_f(JobFlags flags, AsyncTask *parent,
                           AsyncFunctionType<void()> *function,
                           size_t initialContextSize) {
  return swift_task_create_future_f(
      flags, parent, nullptr, function, initialContextSize);
}

AsyncTaskAndContext swift::swift_task_create_future(
    JobFlags flags, AsyncTask *parent, const Metadata *futureResultType,
    const AsyncFunctionPointer<void()> *function) {
  return swift_task_create_future_f(
      flags, parent, futureResultType, function->Function.get(),
      function->ExpectedContextSize);
}

AsyncTaskAndContext swift::swift_task_create_future_f(
    JobFlags flags, AsyncTask *parent, const Metadata *futureResultType,
    AsyncFunctionType<void()> *function, size_t initialContextSize) {
  assert((futureResultType != nullptr) == flags.task_isFuture());
  assert(!flags.task_isFuture() ||
         initialContextSize >= sizeof(FutureAsyncContext));
  assert((parent != nullptr) == flags.task_isChildTask());

  // Figure out the size of the header.
  size_t headerSize = sizeof(AsyncTask);
  if (parent) headerSize += sizeof(AsyncTask::ChildFragment);

  if (futureResultType) {
    headerSize += FutureFragment::fragmentSize(futureResultType);
  }

  headerSize = llvm::alignTo(headerSize, llvm::Align(alignof(AsyncContext)));

  // Allocate the initial context together with the job.
  // This means that we never get rid of this allocation.
  size_t amountToAllocate = headerSize + initialContextSize;

  // TODO: if this is necessary we need to teach LLVM lowering to request async
  // context sizes that are mulitple of that maximum alignment.
  // For now disable this assert.
  // assert(amountToAllocate % MaximumAlignment == 0);

  void *allocation = malloc(amountToAllocate);

  AsyncContext *initialContext =
    reinterpret_cast<AsyncContext*>(
      reinterpret_cast<char*>(allocation) + headerSize);

  // Initialize the task so that resuming it will run the given
  // function on the initial context.
  AsyncTask *task =
    new(allocation) AsyncTask(&taskHeapMetadata, flags,
                              function, initialContext);

  // Initialize the child fragment if applicable.
  // TODO: propagate information from the parent?
  if (parent) {
    auto childFragment = task->childFragment();
    new (childFragment) AsyncTask::ChildFragment(parent);
  }

  // Initialize the future fragment if applicable.
  if (futureResultType) {
    auto futureFragment = task->futureFragment();
    new (futureFragment) FutureFragment(futureResultType);

    // Set up the context for the future so there is no error, and a successful
    // result will be written into the future fragment's storage.
    auto futureContext = static_cast<FutureAsyncContext *>(initialContext);
    futureContext->errorResult = nullptr;
    futureContext->indirectResult = futureFragment->getStoragePtr();
  }

  // Configure the initial context.
  //
  // FIXME: if we store a null pointer here using the standard ABI for
  // signed null pointers, then we'll have to authenticate context pointers
  // as if they might be null, even though the only time they ever might
  // be is the final hop.  Store a signed null instead.
  initialContext->Parent = nullptr;
  initialContext->ResumeParent = &completeTask;
  initialContext->ResumeParentExecutor = ExecutorRef::noPreference();
  initialContext->Flags = AsyncContextKind::Ordinary;
  initialContext->Flags.setShouldNotDeallocateInCallee(true);

  // Initialize the task-local allocator.
  _swift_task_alloc_initialize(task);

  return {task, initialContext};
}

TaskFutureWaitResult
swift::swift_task_future_wait(AsyncTask *task, AsyncTask *waitingTask) {
  assert(task->isFuture());
  switch (task->waitFuture(waitingTask)) {
  case FutureFragment::Status::Executing:
    return TaskFutureWaitResult{TaskFutureWaitResult::Waiting, nullptr};

  case FutureFragment::Status::Success:
    return TaskFutureWaitResult{
        TaskFutureWaitResult::Success, task->futureFragment()->getStoragePtr()};

   case FutureFragment::Status::Error:
      return TaskFutureWaitResult{
          TaskFutureWaitResult::Error,
          reinterpret_cast<OpaqueValue *>(task->futureFragment()->getError())};
  }
}

// TODO: Remove this hack.
void swift::swift_task_run(AsyncTask *taskToRun) {
  taskToRun->run(ExecutorRef::noPreference());
}
