#include <torch/csrc/autograd/engine.h>

#include <torch/csrc/autograd/function.h>
#include <torch/csrc/autograd/functions/basic_ops.h>
#include <torch/csrc/autograd/grad_mode.h>
#include <torch/csrc/autograd/anomaly_mode.h>
#include <torch/csrc/autograd/variable.h>
#include <torch/csrc/utils/memory.h>

#include <ATen/DeviceGuard.h>
#include <ATen/ExpandUtils.h>
#include <ATen/Parallel.h>
#include <c10/util/Exception.h>
#include <c10/core/Stream.h>
#include <c10/core/Event.h>
#include <c10/core/DeviceGuard.h>
#include <c10/util/Optional.h>
#include <c10/core/StreamGuard.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_set>
#include <typeinfo>
#include <sstream>
#include <queue>
#include <TH/TH.h>

namespace torch { namespace autograd {

// Threads spawned by the engine are assigned a constant 'worker_device'
// specifying what device they process work for.  This variable is initialized
// at thread creation time and is constant afterwards.  This is used when
// handling reentrant backwards calls; see Note [Reentrant backwards]
static thread_local int worker_device = NO_DEVICE;

// This variable is true if ALL invocations in the stack of re-entrant engine
// invocations are imperative backwards. This special variable is needed for the
// gradient checkpointing feature only.
static thread_local bool checkpoint_valid = true;

// XXX: Changes to the way multithreading works in execute should be done with
// great care. Right now the implementation guarantees that a single function's
// apply will never be entered concurrently (even if multiple graphs are
// executed at the same time). Adding multiple threads per-device or removing
// engine thread affinity to the device can break this invariant, and we depend
// on it in a few places (e.g. AccumulateGrad function).

// Number of nested reentrant backwards calls currently on this thread
static thread_local int current_depth = 0;
// Total nested reentrant backwards calls over all threads for worker_device
static thread_local int total_depth = 0;

// This shared_ptr is a thread local pointer to the local ready queue per thread
// We colocate each device (i.e. CUDA, XLA) a separate thread other than CPU,
// see Note [Allocating GPUs to autograd threads] each device thread should have
// its own ReadyQueue that initialized in thread_init and used as the queue for
// executing tasks. These local_ready_queues for device threads are also memorized
// in the Engine to perform cross device training (i.e. CPU to GPU, XLA, etc.)
//
// For CPU threads, each thread will also have its own ReadyQueue per thread, and
// it is memorized in the GraphTask to perform cross device training. (i.e. GPU to
// CPU via variable.cpu(), etc.)
//
// For reentrant backward calls, if we spawn new thread from the current thread
// because we reached the maximum depth, the new thread will just reuse the same
// ReadyQueue with the parent thread for a mild performance improvement.
// see Note [Reentrant backwards] for more details.
static thread_local std::shared_ptr<ReadyQueue> local_ready_queue = nullptr;

// Note [Reentrant backwards]
// ~~~~~~~~~~~~~~~~~~~~~~~~~~
// To understand the reentrant backwards problem, we have to notice two
// aspects of how the autograd engine is implemented today:
//
//  1. When you call Engine::execute(), you want to block until
//  differentiation finishes so that you can get the final result variables
//  of the backwards pass.
//
//  2. The engine operates by having a single worker thread per work queue,
//  and every work queue is pinned to a specific device where the
//  operation is executed.
//
// The problem is, suppose that you call backward() inside of a worker
// thread.  By property (1), we're supposed to block until the nested task
// finishes.  However, by property (2), this worker thread is on the
// hook for processing the tasks assigned to it; we better not block,
// because then all of our backward executions (including the one we
// just started) will deadlock!
//
// We maintain a pool of threads waiting for work to do
// When a reentrant backwards call occurs, the current thread blocks
// and a thread from the pool is woken up to complete the blocking tasks and an
// any other tasks that would have been assigned to that worker. If there are no
// threads available, a new thread is spawned. The new thread will continue
// processing tasks from the same ReadyQueue as the parent worker
//
// When the GraphTask is finished, the parent worker thread that is waiting on
// the task is notified and the current thread returns to the pool.

// Note [Streaming backwards]
// ~~~~~~~~~~~~~~~~~~~~~~~~~~
// On CUDA devices the autograd engine's device operations are run on the
// same stream that ran them in forward. This requires automatically
// syncing the streams so that function A finishes producing its
// output before function B consumes it.
//
// This synchronization occurs when outputs are placed into input buffers.
// The functions corresponding to input buffer positions have metadata
// recording their streams from forward, and during backward this
// data is used to sync the producer's stream with the consumer's.
//
// When a CUDA function is run either all its inputs were accumulated on the
// stream used to run the function OR the inputs are on different devices
// and the function is responsible for properly acquiring them.
//
// Historically, the autograd engine ran all CUDA operations on their
// device's DEFAULT stream. This meant that syncing (implicitly or
// explicitly) with the default streams was required before and after
// calling backward(). It also meant, however, that syncing with
// the default streams after backward() was sufficient to ensure
// that backward() had finished running. To preserve this historic
// behavior the engine records "leaf streams," the streams of the
// leaf variables, and syncs them with their device's default stream
// at the end of backward. All other streams are already synchronized
// to happen before at least one leaf stream (per the above), so syncing
// the leaf streams with the default streams is sufficient to implement
// the historic behavior.

int NodeTask::getReentrantDepth() const {
  std::shared_ptr<GraphTask> graph_task = base_.lock();
  if (graph_task) {
    return graph_task->reentrant_depth_;
  } else {
    // The graph task is no longer valid indicating an error. As a result, we
    // try to move this to the front of the queue to ensure the autograd
    // engine threads pick up this error soon.
    return std::numeric_limits<int>::max();
  }
}

bool graph_task_completed(const std::shared_ptr<GraphTask>& graph_task) {
  return graph_task->outstanding_tasks_.load() == 0 ||
      (graph_task->exit_on_error_ && graph_task->has_error_.load());
}

auto ReadyQueue::push(NodeTask item, bool incrementOutstandingTasks) -> void {
  {
    // Lock mutex for writing to heap_
    std::lock_guard<std::mutex> lock(mutex_);
    if (incrementOutstandingTasks) {
      std::shared_ptr<GraphTask> graph_task = item.base_.lock();
      TORCH_INTERNAL_ASSERT(graph_task, "GraphTask is no longer valid!");
      ++graph_task->outstanding_tasks_;
    }
    heap_.push(std::move(item));
  }
  not_empty_.notify_one();
}

auto ReadyQueue::pushShutdownTask() -> void {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    heap_.push(NodeTask({}, nullptr, InputBuffer(0), true));
  }
  not_empty_.notify_one();
}

size_t ReadyQueue::size() const {
  // Lock mutex for accesses to heap_
  std::unique_lock<std::mutex> lock(mutex_);
  return heap_.size();
}

auto ReadyQueue::pop() -> NodeTask {
  // Lock mutex for accesses to heap_
  std::unique_lock<std::mutex> lock(mutex_);
  not_empty_.wait(lock, [this]{ return !heap_.empty(); });
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
  auto task = std::move(const_cast<NodeTask&>(heap_.top())); heap_.pop();
  return task;
}

bool ReadyQueue::empty() const {
  // Lock mutex for accesses to heap_
  std::unique_lock<std::mutex> lock(mutex_);
  return heap_.empty();
}

// This limit is based on the default python recursion limit which is 1000
Engine::Engine() : max_recursion_depth_(100) {}

// Send shutdown tasks to all ReadyQueues if no backward tasks are running
// Even though readyQueue should be empty, shutdown tasks have the highest
// priority
Engine::~Engine() {
  bool noBackward = true;
  for (auto& queue: device_ready_queues_) {
    noBackward =  noBackward && queue->empty();
  }
  if (noBackward) {
    for (auto& queue : device_ready_queues_) {
     queue->pushShutdownTask();
    }
  }
  // Othewise threads are leaked
}

void Engine::set_device(int device) {
  // NB: We MUST NOT construct the guard for device CPU,
  // as in some settings we compile with cuda, but
  // have lazy stubs for CUDA functionality (so actually
  // attempting to setup a guard(CPU_DEVICE) will cause an
  // error, because it will still query cudaGetDevice).
  //
  // Don't use DeviceGuard here because its destructor may be called before the
  // device is reset. This is fine because the device is thread local.
  if (device != CPU_DEVICE) {
    for (size_t i = 0; i < static_cast<size_t>(c10::DeviceType::COMPILE_TIME_MAX_DEVICE_TYPES); i++) {
      auto* impl = c10::impl::device_guard_impl_registry[i].load();
      if (impl && device < impl->deviceCount()) {
        impl->setDevice(at::Device(static_cast<c10::DeviceType>(i), device));
      }
    }
  }
  worker_device = device;
}

auto Engine::thread_init(int device, const std::shared_ptr<ReadyQueue>& ready_queue) -> void {
  at::init_num_threads();
  // thread_init should only be called by device threads other than CPU_DEVICE
  TORCH_INTERNAL_ASSERT(device != CPU_DEVICE);

  // Note [Allocating GPUs to autograd threads]
  // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
  // What's our strategy here?  Originally, the autograd engine was written
  // with only CUDA in mind.  We allocate one thread to handle all CPU
  // operations, and a thread per CUDA device.
  //
  // But what if we have OTHER devices?  There are two plausible
  // strategies:
  //
  //  - We can allocate threads equal to max(num_cuda_devices, num_xla_devices,
  //    ...) and colocate cuda device 0 with xla device 0
  //  - We can allocate threads equal to sum(num_cuda_devices, num_xla_devices,
  //    ...) keeping everyone separate.
  //
  // We don't have any good reason to prefer one or the other, so we've
  // arbitrarily picked to colocate devices.  Maybe the other approach is
  // better.
  set_device(device);

  // initialize each device thread's thread local ready queue with the ready queue
  // that is created before the thread initialization
  init_local_ready_queue(ready_queue);

  std::shared_ptr<GraphTask> graph_task = nullptr;
  thread_main(graph_task, /* reentrant_thread */ false);
}

// NOTE: graph_tasks do not necessarily form a stack. Imagine this
// case:
//
//    +----> Eval1
//  Root
//    +----> Eval2
//
// Once Root is executed, both Eval1 and Eval2 are added to the ready queue.
// Next, Eval1 is run and this causes the worker to enter thread_main again.
// Then, it pops the next task from the queue, but at this point it is Eval2.
// It enters thread_main once again, but now with graph_task of Eval2, which is
// completely unrelated to that of Eval1 (it's not a recursive call).
// It's all ok and is handled right now, but it should be accounted for
// in case this code is to be changed.
auto Engine::thread_main(
    const std::shared_ptr<GraphTask>& graph_task,
    bool reentrant_thread) -> void {
  // Either reentrant_thread should be false or we should pass in a non-null
  // graph_task.
  TORCH_INTERNAL_ASSERT(reentrant_thread != (graph_task == nullptr));

  TORCH_INTERNAL_ASSERT(local_ready_queue != nullptr);
  // Why the test on graph_task->outstanding_tasks_?  See
  // Note [Reentrant backwards]
  while (!reentrant_thread || graph_task->outstanding_tasks_ > 0) {
    NodeTask task = local_ready_queue->pop();
    // This will only work if the worker is running a non backward task
    // TODO Needs to be fixed this to work in all cases
    if (task.isShutdownTask_) {
      C10_LOG_API_USAGE_ONCE("torch.autograd.thread_shutdown");
      break;
    }

    // local_graph_task represents the graph_task we retrieve from the queue.
    // The outer graph_task represents the overall graph_task we need to execute
    // for reentrant execution.
    std::shared_ptr<GraphTask> local_graph_task;
    if (!(local_graph_task = task.base_.lock())) {
      // Reentrant thread's graph task should not expire since we hold a
      // reference to it in this method.
      TORCH_INTERNAL_ASSERT(!reentrant_thread);
      LOG(INFO) << "GraphTask for function " << task.fn_->name()
                << " is no longer valid, skipping execution";
      continue;
    }

    if (task.fn_ && !local_graph_task->has_error_.load()) {
      AutoGradMode grad_mode(local_graph_task->grad_mode_);
      try {
        evaluate_function(local_graph_task, task.fn_.get(), task.inputs_);
      } catch (std::exception& e) {
        thread_on_exception(local_graph_task, task.fn_, e);
      }
    }

    // Decrement the outstanding tasks.
    --local_graph_task->outstanding_tasks_;

    // Check if we've completed execution.
    bool gt_completed = graph_task_completed(local_graph_task);
    if (gt_completed) {
      // We don't need to explicitly notify the owner thread, since
      // 'mark_graph_task_completed' would mark the Future as completed and this
      // would notify the owner thread that the task has been completed.
      mark_graph_task_completed(local_graph_task);

      // The CPU worker thread is actually the thread that initially requested
      // the autograd computation. Now that we're done, we need to break out of
      // the worker loop so we can continue executing the rest of the calling code!
      if (worker_device == CPU_DEVICE) {
        break;
      }
    }

    auto base_owner = local_graph_task->owner_;
    // Send a dummy function task to the owning thread just to
    // ensure that it's not sleeping. If it has work, it might see that
    // graph_task->outstanding_tasks_ == 0 before it gets to the task, but
    // it's a no-op anyway.
    // This is not necessary if the current thread is the owning thread.
    if (base_owner != worker_device && gt_completed) {
      // Synchronize outstanding_tasks_ with queue mutex
      std::atomic_thread_fence(std::memory_order_release);
      ready_queue_by_index(local_graph_task, base_owner)
          .push(NodeTask(local_graph_task, nullptr, InputBuffer(0)));
    }
  }
}

void Engine::reentrant_thread_init(const std::shared_ptr<ReadyQueue>& parent_ready_queue) {
  at::init_num_threads();
  auto tp_shared= thread_pool_shared_;
  while(true) {
    std::unique_lock<std::mutex> lk(tp_shared->mutex_);
    ++thread_pool_shared_->num_workers_;
    tp_shared->work_.wait(lk, [&tp_shared]{ return !tp_shared->graphtasks_queue_.empty();});
    --thread_pool_shared_->num_workers_;
    auto task = tp_shared->graphtasks_queue_.front();
    tp_shared->graphtasks_queue_.pop();
    lk.unlock();
    std::shared_ptr<GraphTask> graph_task;
    if (!(graph_task = task.lock())) {
      LOG(INFO) << "GraphTask has expired, skipping reentrant execution";
      continue;
    }
    set_device(graph_task->owner_);
    init_local_ready_queue(parent_ready_queue);
    total_depth = graph_task->reentrant_depth_;
    thread_main(graph_task, /* reentrant thread*/ true);
  }
}

void Engine::thread_on_exception(
    std::shared_ptr<GraphTask>& graph_task,
    const std::shared_ptr<Node>& fn,
    std::exception& e) {
  graph_task->set_exception(e, fn);
}

void GraphTask::set_exception(
    std::exception& e,
    const std::shared_ptr<Node>& fn) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!has_error_.load()) {
    if (AnomalyMode::is_enabled() && fn) {
      fn->metadata()->print_stack();
    }
    has_error_ = true;
    if (!future_result_->completed()) {
      future_result_->setError(e.what());
    } else {
      TORCH_INTERNAL_ASSERT(future_result_->hasError());
    }
  }
}

static variable_list call_pre_hooks(Node& fn, variable_list inputs) {
  for (const auto& hook : fn.pre_hooks()) {
    inputs = (*hook)(inputs);
  }
  return inputs;
}

static variable_list call_post_hooks(Node& fn, variable_list outputs, const variable_list& inputs) {
  for (const auto& hook : fn.post_hooks()) {
    outputs = (*hook)(outputs, inputs);
  }
  return outputs;
}

static bool is_compatible_type(const at::TensorOptions& expected, const at::TensorOptions& actual) {
  // Types are compatible if they exactly match or if the gradient is a sparse
  // version of the expected type.
  return expected.type_equal(actual) || (actual.is_sparse() && expected.device().type() == actual.device().type());
}

void validate_outputs(
    const edge_list& edges,
    variable_list& grads,
    const std::function<std::string(const std::string&)>& format_error) {
  if (grads.size() != edges.size()) {
    std::stringstream ss;
    ss << "invalid number of gradients - expected ";
    ss << edges.size() << ", but got " << grads.size();
    AT_ERROR(format_error(ss.str()));
  }
  for (size_t i = 0; i < grads.size(); i++) {
    const auto& edge = edges[i];
    if (!edge.is_valid()) continue;

    const auto& metadata = edge.function->input_metadata(edge.input_nr);
    const auto& output = grads[i];
    if (!output.defined()) {
      // FIXME: TestJit.test_ge_optimized fails this assertion.
      // std::stringstream ss;
      // ss << "undefined gradient at index " << i;
      // AT_ERROR(format_error(ss.str()));
      continue;
    }
    if (!grads[i].sizes().equals(metadata.shape())) {
      if (!at::is_expandable_to(metadata.shape(), grads[i].sizes())) {
        std::stringstream ss;
        ss << "invalid gradient at index " << i << " - got ";
        ss << grads[i].sizes() << " but expected shape compatible with ";
        ss << metadata.shape();
        AT_ERROR(format_error(ss.str()));
      }
      grads[i] = at::sum_to(std::move(grads[i]), metadata.shape());
    }
    TORCH_CHECK(isFloatingType(grads[i].scalar_type()));
    if (c10::typeMetaToScalarType(metadata.options().dtype()) != grads[i].scalar_type()) {
      grads[i] = grads[i].to(c10::typeMetaToScalarType(metadata.options().dtype()));
    }
    if (!is_compatible_type(metadata.options(), grads[i].options())) {
       std::stringstream ss;
       ss << "invalid gradient at index " << i << " - expected type ";
       ss << metadata.options() << " but got " << grads[i].options();
       AT_ERROR(format_error(ss.str()));
    }
    auto output_device = output.device();
    if (output_device != metadata.device()) {
      std::stringstream ss;
      ss << "invalid gradient at index " << i << " - expected device ";
      ss << metadata.device() << " but got " << output_device;
      AT_ERROR(format_error(ss.str()));
    }
  }
}

static variable_list call_function(
    std::shared_ptr<GraphTask>& graph_task,
    Node* func,
    InputBuffer& inputBuffer) {
  bool prev_checkpoint_valid_state = checkpoint_valid;
  checkpoint_valid =
      graph_task->can_checkpoint() && prev_checkpoint_valid_state;
  auto& fn = *func;
  auto inputs =
      call_pre_hooks(fn, InputBuffer::variables(std::move(inputBuffer)));

  if (!graph_task->keep_graph_) {
    fn.will_release_variables();
  }

  const auto has_post_hooks = !fn.post_hooks().empty();
  variable_list outputs;

  {
    at::DebugInfoGuard guard(graph_task->debug_info_);
    if (has_post_hooks) {
      // In functions/accumulate_grad.cpp, there is some logic to check the
      // conditions under which the incoming gradient can be stolen directly
      // (which elides a deep copy) instead of cloned. One of these conditions
      // is that the incoming gradient's refcount must be 1 (nothing else is
      // referencing the same data).  Stashing inputs_copy here bumps the
      // refcount, so if post hooks are employed, it's actually still ok for
      // accumulate_grad.cpp to steal the gradient if the refcount is 2.
      //
      // "new_grad.use_count() <= 1 + !post_hooks().empty()" in
      // accumulate_grad.cpp accounts for this, but also creates a silent
      // dependency between engine.cpp (ie, this particular engine
      // implementation) and accumulate_grad.cpp.
      //
      // If you change the logic here, make sure it's compatible with
      // accumulate_grad.cpp.
      auto inputs_copy = inputs;
      outputs = fn(std::move(inputs_copy));
    } else {
      outputs = fn(std::move(inputs));
    }
  }

  validate_outputs(fn.next_edges(), outputs, [&](const std::string& msg) {
    std::ostringstream ss;
    ss << "Function "  << fn.name() << " returned an " << msg;
    return ss.str();
  });
  checkpoint_valid = prev_checkpoint_valid_state;

  if(has_post_hooks){
    // NOLINTNEXTLINE(bugprone-use-after-move)
    return call_post_hooks(fn, std::move(outputs), inputs);
  }
  return outputs;
}

void Engine::evaluate_function(
    std::shared_ptr<GraphTask>& graph_task,
    Node* func,
    InputBuffer& inputs) {
  // If exec_info_ is not empty, we have to instrument the execution
  auto& exec_info_ = graph_task->exec_info_;
  if (!exec_info_.empty()) {
    auto& fn_info = exec_info_.at(func);
    if (auto* capture_vec = fn_info.captures_.get()) {
      // Lock mutex for writing to graph_task->captured_vars_.
      std::lock_guard<std::mutex> lock(graph_task->mutex_);
      for (auto capture : *capture_vec) {
        graph_task->captured_vars_[capture.output_idx_] =
            inputs[capture.input_idx_];
      }
    }
    if (!fn_info.needed_) {
      // Skip execution if we don't need to execute the function.
      return;
    }
  }

  // Switches to a function's CUDA stream (if applicable) before calling it
  const auto opt_parent_stream = (*func).stream(c10::DeviceType::CUDA);
  c10::OptionalStreamGuard parent_stream_guard{opt_parent_stream};

  auto outputs = call_function(graph_task, func, inputs);

  auto& fn = *func;
  if (!graph_task->keep_graph_) {
    fn.release_variables();
  }

  int num_outputs = outputs.size();
  if (num_outputs == 0) { // Note: doesn't acquire the mutex
    // Records leaf stream (if applicable)
    // See note "Streaming backwards"
    if (opt_parent_stream) {
      std::lock_guard<std::mutex> lock(graph_task->mutex_);
      graph_task->leaf_streams.emplace(*opt_parent_stream);
    }
    return;
  }

  if (AnomalyMode::is_enabled()) {
    AutoGradMode grad_mode(false);
    for (int i = 0; i < num_outputs; ++i) {
      auto& output = outputs[i];
      at::OptionalDeviceGuard guard(device_of(output));
      if (output.defined() && isnan(output).any().item<uint8_t>()) {
        std::stringstream ss;
        ss << "Function '" << fn.name() << "' returned nan values in its " << i << "th output.";
        throw std::runtime_error(ss.str());
      }
    }
  }

  // Lock mutex for the accesses to GraphTask dependencies_, not_ready_ and cpu_ready_queue_ below
  std::lock_guard<std::mutex> lock(graph_task->mutex_);
  for (int i = 0; i < num_outputs; ++i) {
    auto& output = outputs[i];
    const auto& next = fn.next_edge(i);

    if (!next.is_valid()) continue;

    // Check if the next function is ready to be computed
    bool is_ready = false;
    auto& dependencies = graph_task->dependencies_;
    auto it = dependencies.find(next.function.get());

    if (it == dependencies.end()) {
      auto name = next.function->name();
      throw std::runtime_error(std::string("dependency not found for ") + name);
    } else if (--it->second == 0) {
      dependencies.erase(it);
      is_ready = true;
    }

    auto& not_ready = graph_task->not_ready_;
    auto not_ready_it = not_ready.find(next.function.get());
    if (not_ready_it == not_ready.end()) {
      // Skip functions that aren't supposed to be executed
      if (!exec_info_.empty()) {
        auto it = exec_info_.find(next.function.get());
        if (it == exec_info_.end() || !it->second.should_execute()) {
          continue;
        }
      }
      // No buffers have been allocated for the function
      InputBuffer input_buffer(next.function->num_inputs());

      // Accumulates into buffer
      const auto opt_next_stream = next.function->stream(c10::DeviceType::CUDA);
      input_buffer.add(next.input_nr,
                       std::move(output),
                       opt_parent_stream,
                       opt_next_stream);

      if (is_ready) {
        auto& queue = ready_queue(graph_task, input_buffer.device());
        queue.push(
            NodeTask(graph_task, next.function, std::move(input_buffer)));
      } else {
        not_ready.emplace(next.function.get(), std::move(input_buffer));
      }
    } else {
      // The function already has a buffer
      auto &input_buffer = not_ready_it->second;

      // Accumulates into buffer
      const auto opt_next_stream = next.function->stream(c10::DeviceType::CUDA);
      input_buffer.add(next.input_nr,
                       std::move(output),
                       opt_parent_stream,
                       opt_next_stream);
      if (is_ready) {
        auto& queue = ready_queue(graph_task, input_buffer.device());
        queue.push(
            NodeTask(graph_task, next.function, std::move(input_buffer)));
        not_ready.erase(not_ready_it);
      }
    }
  }
}

/* Computes the number of dependencies for each function which requires grad */
auto Engine::compute_dependencies(Node* root, GraphTask& task) -> void {
  // Just to make sure that they will never be added to the queue again
  std::unordered_set<Node*> seen;
  std::vector<Node*> queue { root };

  // Queue contains all nodes that will start propagating gradients.
  // We no longer have to expand functions that don't require grad.
  auto& dependencies = task.dependencies_;
  while (!queue.empty()) {
    auto fn = queue.back(); queue.pop_back();
    for (const auto& edge : fn->next_edges()) {
      if (auto next_ptr = edge.function.get()) {
        dependencies[next_ptr] += 1;
        const bool was_inserted = seen.insert(next_ptr).second;
        if (was_inserted) queue.push_back(next_ptr);
      }
    }
  }
}

struct ClearCallbacks {
  ClearCallbacks(std::vector<std::function<void()>>& callbacks,
                 std::mutex &callbacks_lock)
    : callbacks_(callbacks)
    , callbacks_lock_(callbacks_lock) { clear(); }
  ~ClearCallbacks() { clear(); }

  void clear() {
    std::lock_guard<std::mutex> lock(callbacks_lock_);
    callbacks_.clear();
  }

  std::vector<std::function<void()>>& callbacks_;
  std::mutex& callbacks_lock_;
};

auto Engine::execute(const edge_list& roots,
                     const variable_list& inputs,
                     bool keep_graph,
                     bool create_graph,
                     const edge_list& outputs) -> variable_list {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
  validate_outputs(roots, const_cast<variable_list&>(inputs), [](const std::string& msg) {
    return msg;
  });

  // Callbacks are only valid for the duration of this run and should always be cleared
  // Lock post_callbacks_lock_ before clearing final_callbacks_
  ClearCallbacks _cb_guard(final_callbacks_, post_callbacks_lock_);

  bool is_reentrant_call = worker_device != NO_DEVICE;
  std::shared_ptr<ReadyQueue> memorized_cpu_ready_queue = nullptr;

  if (is_reentrant_call) {
    // Reentrant call will still use the parent thread ready_queue
    // While we can create separate cpu_ready_queue for each new reentrant
    // thread, but sharing the same cpu_ready_queue with parent thread is
    // a mild performance improvement and cuda thread still have to do the
    // same thing.
    memorized_cpu_ready_queue = local_ready_queue;
  } else {
    // not a reentrant call, Engine::execute should start on CPU device
    // create the thread local ready queue on CPU and memorize it in GraphTask
    init_local_ready_queue(std::make_shared<ReadyQueue>());
    memorized_cpu_ready_queue = local_ready_queue;
  }

  auto graph_task = std::make_shared<GraphTask>(
      keep_graph,
      create_graph,
      worker_device == NO_DEVICE ? 0 : total_depth + 1,
      memorized_cpu_ready_queue);

  // Now compute the dependencies for all executable functions and queue the root
  auto graph_root = std::make_shared<GraphRoot>(roots, inputs);
  compute_dependencies(graph_root.get(), *graph_task);

  if (!outputs.empty()) {
    graph_task->init_to_execute(*graph_root, outputs);
  }

  return execute_with_graph_task(graph_task, graph_root);
}

void Engine::enqueue_blocked_task_on_cpu(NodeTask task) {
  std::call_once(start_device_threads_flag_, &Engine::start_device_threads, this);
  std::shared_ptr<GraphTask> graph_task = task.base_.lock();
  // The graph_task must be alive at this point, because internal autograd machinary
  // who calls this API (Distributed Autograd Engine) increases outstanding_tasks_
  // outside this API to keep the GraphTask alive
  TORCH_INTERNAL_ASSERT(graph_task, "GraphTask is no longer valid!");
  ready_queue(graph_task, at::kCPU).push(
      std::move(task), /* incrementOutstandingTasks */ false);
}

variable_list Engine::execute_with_graph_task(
    const std::shared_ptr<GraphTask>& graph_task,
    std::shared_ptr<Node> graph_root) {
  std::call_once(start_device_threads_flag_, &Engine::start_device_threads, this);
  // Lock mutex for GraphTask.
  std::unique_lock<std::mutex> lock(graph_task->mutex_);

  ready_queue(graph_task, at::kCPU).push(
      NodeTask(graph_task, std::move(graph_root), InputBuffer(0)));

  // worker_device == NO_DEVICE it's a CPU thread and it's trying to drive the
  // autograd engine with corresponding GraphTask, and its NOT a re-entrant call
  if (worker_device == NO_DEVICE) {
    // graph_task_exec_post_processing is done when the Future is marked as
    // completed in mark_graph_task_completed.

    // We set the worker_device to CPU_DEVICE only if worker_device was previously
    // NO_DEVICE. Setting it to CPU afterwards allow us to detect whether this is
    // a re-entrant call or not.
    //
    // If worker_device = NO_DEVICE: this is not a re-entrant call
    // If worker_device is other devices (i.e. CPU, CUDA): this is a re-entrant
    //    backward call from that device.
    set_device(CPU_DEVICE);

    // set the graph_task owner to the current device
    graph_task->owner_ = worker_device;

    // The owning thread start to drive the engine execution with the GraphTask
    // that has already been pushed to the current CPU thread's ready_queue
    lock.unlock();
    thread_main(nullptr, false);

    // reset the worker_device and the cpu_ready_queue after the completion of the
    // graph_task, this is so that the initial state of the engine remains the
    // same across every backward()/grad() call.
    worker_device = NO_DEVICE;
    local_ready_queue.reset();
    return graph_task->future_result_->wait();
  } else {
    // this must be a re-entrant call from worker_device
    graph_task->owner_ = worker_device;
    if (current_depth >= max_recursion_depth_) {
      // See Note [Reentrant backwards]
      // If reached the max depth, switch to a different thread
      lock.unlock();
      add_thread_pool_task(graph_task);
      return graph_task->future_result_->wait();
    } else {
      // Total depth needs to be updated only in this codepath, since it is
      // not used in the block above (when we call add_thread_pool_task).
      // In the codepath above, GraphTask.reentrant_depth_ is used to
      // bootstrap total_depth in the other thread.
      ++total_depth;

      // Get back to work while we wait for our new graph_task to
      // complete!
      ++current_depth;
      lock.unlock();
      thread_main(graph_task, /* reentrant_thread */ true);
      --current_depth;
      --total_depth;

      TORCH_INTERNAL_ASSERT(graph_task->future_result_->completed());

      return graph_task->future_result_->wait();
    }
  }
  // // The graph task should have completed and the associated future should
  // // be marked completed as well.
  // TORCH_INTERNAL_ASSERT(graph_task->future_result_->completed());

  // // graph_task_exec_post_processing is done when the Future is marked as
  // // completed in mark_graph_task_completed.
  // return graph_task->future_result_->wait();
}

void Engine::mark_graph_task_completed(std::shared_ptr<GraphTask>& graph_task) {
  std::unique_lock<std::mutex> lock(graph_task->mutex_);
  if (graph_task->future_result_->completed()) {
    // Future is already marked as completed.
    return;
  }

  try {
    auto val = graph_task_exec_post_processing(graph_task);
    graph_task->future_result_->markCompleted(val);
  } catch (std::exception& e) {
    graph_task->future_result_->setError(e.what());
  }
}

variable_list Engine::graph_task_exec_post_processing(
    const std::shared_ptr<GraphTask>& graph_task) {
  if (!graph_task->not_ready_.empty()) {
    throw std::runtime_error("could not compute gradients for some functions");
  }

  // Lock mutex during each iteration for accessing final_callbacks.size()
  // Unlocking is necessary, because the callback can register
  // more callbacks (or they can be registered from other threads
  // while it's waiting.
  std::unique_lock<std::mutex> cb_lock(post_callbacks_lock_);
  // WARNING: Don't use a range-for loop here because more callbacks may be
  // added in between callback calls, so iterators may become invalidated.
  // NOLINTNEXTLINE(modernize-loop-convert)
  for (size_t i = 0; i < final_callbacks_.size(); ++i) {
    cb_lock.unlock();
    final_callbacks_[i]();
    cb_lock.lock();
  }

  // Syncs leaf streams with default streams (if necessary)
  // See note "Streaming backwards"
  for (const auto& leaf_stream : graph_task->leaf_streams) {
    const auto guard = c10::impl::VirtualGuardImpl{c10::DeviceType::CUDA};
    const auto default_stream = guard.getDefaultStream(leaf_stream.device());
    if (leaf_stream != default_stream) {
      auto event = c10::Event{c10::DeviceType::CUDA};
      event.record(leaf_stream);
      default_stream.wait(event);
    }
  }

  return graph_task->captured_vars_;
}


// note that when python is present, this base engine will be overriden
// with a PythonEngine. Because this typically happens before get_default_engine
// is called, this base engine will never be created.
static Engine& get_base_engine() {
  static Engine engine;
  return engine;
}

std::atomic<EngineStub> engine_stub(get_base_engine);

void set_default_engine_stub(EngineStub stub) {
  engine_stub.store(stub);
}


Engine& Engine::get_default_engine() {
  return engine_stub.load()();
}

void Engine::queue_callback(std::function<void()> callback) {
  std::lock_guard<std::mutex> lock(post_callbacks_lock_);
  final_callbacks_.emplace_back(std::move(callback));
}

bool Engine::is_checkpoint_valid() {
  return checkpoint_valid;
}

void Engine::init_local_ready_queue(std::shared_ptr<ReadyQueue> ready_queue) {
  TORCH_INTERNAL_ASSERT(!local_ready_queue);
  local_ready_queue = std::move(ready_queue);
}

size_t Engine::ready_queue_size(const std::shared_ptr<GraphTask>& graph_task, at::Device device) {
  if (device_ready_queues_.empty()) {
    // The vector ready_queues_ is initialized in start_threads, but this method
    // can be called before start_threads. Adding this check to avoid index
    // out of bound error.
    return 0;
  }
  return ready_queue(graph_task, device).size();
}

// CPU ready queue is per GraphTask, but CUDA device ready queues are shared across all graph tasks
auto Engine::ready_queue(const std::shared_ptr<GraphTask>& graph_task, at::Device device) -> ReadyQueue& {
  if (device.type() == at::kCPU) {
    // return the cpu ready queue memorized in GraphTask
    TORCH_INTERNAL_ASSERT(graph_task);
    TORCH_INTERNAL_ASSERT(graph_task->cpu_ready_queue_ != nullptr);
    return *graph_task->cpu_ready_queue_;
  } else {
    // See Note [Allocating GPUs to autograd threads]
    return *device_ready_queues_.at(device.index());
  }
}

// See Note [Allocating GPUs to autograd threads]
// NB: This would become obsolete if we truly allocated a CPU thread
// per device, rather than colocate.
auto Engine::ready_queue_by_index(const std::shared_ptr<GraphTask>& graph_task, int device_index) -> ReadyQueue& {
  if (device_index == CPU_DEVICE) {
    // return the cpu ready queue memorized in GraphTask
    TORCH_INTERNAL_ASSERT(graph_task);
    TORCH_INTERNAL_ASSERT(graph_task->cpu_ready_queue_ != nullptr);
    return *graph_task->cpu_ready_queue_;
  } else {
    return *device_ready_queues_.at(device_index);
  }
}

auto Engine::start_device_threads() -> void {
  // See Note [Allocating GPUs to autograd threads]
  c10::DeviceIndex num_devices = 0;
  for (const auto& impl_atomic : c10::impl::device_guard_impl_registry) {
    auto* impl = impl_atomic.load();
    if (impl) {
      num_devices = std::max(num_devices, impl->deviceCount());
    }
  }

  // allocate one thread for every GPU device (but colocate GPUs of different
  // types), and pre-allocate the device_ready_queues_ to ensure safe reading on it.
  device_ready_queues_ = std::vector<std::shared_ptr<ReadyQueue>>(num_devices);
  for (auto& queue : device_ready_queues_)	{
    queue.reset(new ReadyQueue());
  }

  thread_pool_shared_ = std::make_shared<ThreadPoolShared>();

  for (int i = 0; i < num_devices; ++i) {
    std::thread t(&Engine::thread_init, this, i, device_ready_queues_[i]);
    t.detach();
  }
}

void Engine::add_thread_pool_task(const std::weak_ptr<GraphTask>& graph_task) {
  std::unique_lock<std::mutex> lck(thread_pool_shared_->mutex_);
  // There may already be some items on the graphtasks_queue_ added by other
  // threads but not enough workers to get to the the new task that will be
  // added
  bool create_thread = (thread_pool_shared_->num_workers_ <= thread_pool_shared_->graphtasks_queue_.size());
  thread_pool_shared_->graphtasks_queue_.push(graph_task);
  // Don't need to be holding the lock while actually creating the thread
  lck.unlock();
  if (create_thread) {
    std::thread t(&Engine::reentrant_thread_init, this, local_ready_queue);
    t.detach();
  }
  // This works even if new thread is created because wait() will test the
  // predicate before waiting
  thread_pool_shared_->work_.notify_one();
}

void GraphTask::init_to_execute(Node& graph_root, const edge_list& outputs) {
  exec_info_[&graph_root].needed_ = true;

  int output_idx = 0;
  for (auto & output_edge : outputs) {
    Node *output = output_edge.function.get();
    auto & info = exec_info_[output];
    if (!info.captures_)
      info.captures_ = make_unique<std::vector<ExecInfo::Capture>>();
    info.captures_->emplace_back(output_edge.input_nr, output_idx++);
  }
  captured_vars_.resize(output_idx);

  // NB: this is an uglier version (recursion replaced with iteration) of the following code:
  // is_needed = {}
  // def compute_is_needed(fn):
  //   if fn not in is_needed:
  //     is_needed[fn] = any(compute_is_needed(next_edge)
  //                         for next_edge in fn.next_edges)
  //   return is_needed[fn]
  struct Frame {
    Frame (Node *fn) : fn_(fn), next_next_fn_(0) {}
    Node *fn_;
    size_t next_next_fn_;

    Node* get_next_fn() {
      const auto & next = fn_->next_edges();
      auto num_next = next.size();
      while (next_next_fn_ < num_next) {
        auto fn = next[next_next_fn_++].function.get();
        if (fn) return fn;
      }
      return nullptr;
    }
  };
  std::vector<Frame> stack;
  std::unordered_set<Node*> seen;
  for (const auto & input : graph_root.next_edges()) {
    if (seen.count(input.function.get()) > 0) continue;
    stack.emplace_back(input.function.get());
    while (!stack.empty()) {
      auto &frame = stack.back();
      if (Node *next_fn = frame.get_next_fn()) {
        if (/* bool unseen = */ seen.emplace(next_fn).second) {
          stack.emplace_back(next_fn);
          continue; // recurse
        }
      } else {
        // NB: if we were using real recursion we could have saved some lookups
        // using a return value from recursive call. It would make this manually unrolled
        // version a lot more complicated, so I skipped that.
        const auto & next_edges = frame.fn_->next_edges();
        const bool needed = std::any_of(
            next_edges.begin(), next_edges.end(), [&](const Edge& edge) {
              auto it = exec_info_.find(edge.function.get());
              return it != exec_info_.end() && it->second.should_execute();
            });
        exec_info_[frame.fn_].needed_ = needed;
        stack.pop_back();
      }
    }
  }
}

}} // namespace torch::autograd
