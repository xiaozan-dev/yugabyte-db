// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "yb/rpc/reactor.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <functional>
#include <mutex>
#include <string>

#include <ev++.h>

#include <glog/logging.h>

#include <boost/scope_exit.hpp>

#include "yb/gutil/ref_counted.h"
#include "yb/gutil/stringprintf.h"
#include "yb/rpc/connection.h"
#include "yb/rpc/cql_rpc.h"
#include "yb/rpc/messenger.h"
#include "yb/rpc/negotiation.h"
#include "yb/rpc/redis_rpc.h"
#include "yb/rpc/rpc_controller.h"
#include "yb/rpc/rpc_introspection.pb.h"
#include "yb/rpc/sasl_client.h"
#include "yb/rpc/sasl_server.h"
#include "yb/rpc/yb_rpc.h"

#include "yb/util/countdown_latch.h"
#include "yb/util/errno.h"
#include "yb/util/flag_tags.h"
#include "yb/util/memory/memory.h"
#include "yb/util/monotime.h"
#include "yb/util/thread.h"
#include "yb/util/threadpool.h"
#include "yb/util/thread_restrictions.h"
#include "yb/util/trace.h"
#include "yb/util/status.h"
#include "yb/util/net/socket.h"

using std::string;
using std::shared_ptr;

DECLARE_string(local_ip_for_outbound_sockets);
DECLARE_int32(num_connections_to_server);
DEFINE_int64(rpc_negotiation_timeout_ms, 3000,
             "Timeout for negotiating an RPC connection.");
TAG_FLAG(rpc_negotiation_timeout_ms, advanced);
TAG_FLAG(rpc_negotiation_timeout_ms, runtime);

namespace yb {
namespace rpc {

namespace {
Status ShutdownError(bool aborted) {
  const char* msg = "reactor is shutting down";
  return aborted ?
      STATUS(Aborted, msg, "", ESHUTDOWN) :
      STATUS(ServiceUnavailable, msg, "", ESHUTDOWN);
}

ConnectionContext* MakeNewConnectionContext(ConnectionType connection_type) {
  switch (connection_type) {
    case ConnectionType::YB:
      return new YBConnectionContext();

    case ConnectionType::REDIS:
      return new RedisConnectionContext();

    case ConnectionType::CQL:
      return new CQLConnectionContext();
  }

  LOG(FATAL) << "Unknown connection type " << yb::util::to_underlying(connection_type);
}

ConnectionPtr MakeNewConnection(ConnectionType connection_type,
                              Reactor* reactor,
                              const Endpoint& remote,
                              int socket,
                              Connection::Direction direction) {
  std::unique_ptr<ConnectionContext> context(MakeNewConnectionContext(connection_type));
  return std::make_shared<Connection>(reactor, remote, socket, direction, move(context));
}

} // anonymous namespace

Reactor::Reactor(const shared_ptr<Messenger>& messenger,
                 int index,
                 const MessengerBuilder &bld)
  : messenger_(messenger),
    name_(StringPrintf("%s_R%03d", messenger->name().c_str(), index)),
    loop_(kDefaultLibEvFlags),
    cur_time_(MonoTime::Now(MonoTime::COARSE)),
    last_unused_tcp_scan_(cur_time_),
    connection_keepalive_time_(bld.connection_keepalive_time()),
    coarse_timer_granularity_(bld.coarse_timer_granularity()) {
  LOG(INFO) << "Create reactor with keep alive_time: " << connection_keepalive_time_.ToString()
            << ", coarse timer granularity: " << coarse_timer_granularity_.ToString();

  process_outbound_queue_task_ =
      MakeFunctorReactorTask(std::bind(&Reactor::ProcessOutboundQueue, this));
}

Status Reactor::Init() {
  DCHECK(thread_.get() == nullptr) << "Already started";
  DVLOG(6) << "Called Reactor::Init()";
  // Register to get async notifications in our epoll loop.
  async_.set(loop_);
  async_.set<Reactor, &Reactor::AsyncHandler>(this);
  async_.start();

  // Register the timer watcher.
  // The timer is used for closing old TCP connections and applying
  // backpressure.
  timer_.set(loop_);
  timer_.set<Reactor, &Reactor::TimerHandler>(this);
  timer_.start(coarse_timer_granularity_.ToSeconds(),
               coarse_timer_granularity_.ToSeconds());

  // Create Reactor thread.
  const std::string group_name = messenger_->name() + "_reactor";
  return yb::Thread::Create(group_name, group_name, &Reactor::RunThread, this, &thread_);
}

void Reactor::Shutdown() {
  {
    std::lock_guard<simple_spinlock> l(pending_tasks_lock_);
    if (closing_) {
      return;
    }
    closing_ = true;
  }

  VLOG(1) << name() << ": shutting down Reactor thread.";
  WakeThread();
}

void Reactor::ShutdownInternal() {
  DCHECK(IsCurrentThread());

  stopping_ = true;

  // Tear down any outbound TCP connections.
  Status service_unavailable = ShutdownError(false);
  VLOG(1) << name() << ": tearing down outbound TCP connections...";
  decltype(client_conns_) client_conns = std::move(client_conns_);
  for (auto& pair : client_conns) {
    const ConnectionPtr& conn = pair.second;
    VLOG(1) << name() << ": shutting down " << conn->ToString();
    conn->Shutdown(service_unavailable);
    if (!conn->context().ReadyToStop()) {
      waiting_conns_.push_back(conn);
    }
  }
  client_conns.clear();

  // Tear down any inbound TCP connections.
  VLOG(1) << name() << ": tearing down inbound TCP connections...";
  for (const ConnectionPtr& conn : server_conns_) {
    VLOG(1) << name() << ": shutting down " << conn->ToString();
    conn->Shutdown(service_unavailable);
    if (!conn->context().ReadyToStop()) {
      LOG(INFO) << "Waiting for " << conn.get();
      waiting_conns_.push_back(conn);
    }
  }
  server_conns_.clear();

  // Abort any scheduled tasks.
  //
  // These won't be found in the Reactor's list of pending tasks
  // because they've been "run" (that is, they've been scheduled).
  Status aborted = ShutdownError(true); // aborted
  for (const auto& task : scheduled_tasks_) {
    task->Abort(aborted);
  }
  scheduled_tasks_.clear();

  for (const auto& task : async_handler_tasks_) {
    task->Abort(aborted);
  }


  {
    std::lock_guard<simple_spinlock> lock(outbound_queue_lock_);
    outbound_queue_stopped_ = true;
    outbound_queue_.swap(processing_outbound_queue_);
  }

  for (auto& call : processing_outbound_queue_) {
    call->Transferred(aborted);
  }
  processing_outbound_queue_.clear();
}

ReactorTask::ReactorTask() {
}

ReactorTask::~ReactorTask() {
}

Status Reactor::GetMetrics(ReactorMetrics *metrics) {
  return RunOnReactorThread([metrics](Reactor* reactor) {
    metrics->num_client_connections_ = reactor->client_conns_.size();
    metrics->num_server_connections_ = reactor->server_conns_.size();
    return Status::OK();
  });
}

void Reactor::QueueEventOnAllConnections(ServerEventListPtr server_event) {
  ScheduleReactorFunctor([server_event = std::move(server_event)](Reactor* reactor) {
    for (const ConnectionPtr& conn : reactor->server_conns_) {
      conn->QueueOutboundData(server_event);
    }
  });
}

Status Reactor::DumpRunningRpcs(const DumpRunningRpcsRequestPB& req,
                                DumpRunningRpcsResponsePB* resp) {
  return RunOnReactorThread([&req, resp](Reactor* reactor) {
    for (const ConnectionPtr& conn : reactor->server_conns_) {
      RETURN_NOT_OK(conn->DumpPB(req, resp->add_inbound_connections()));
    }
    for (const conn_map_t::value_type& entry : reactor->client_conns_) {
      Connection* conn = entry.second.get();
      RETURN_NOT_OK(conn->DumpPB(req, resp->add_outbound_connections()));
    }
    return Status::OK();
  });
}

void Reactor::WakeThread() {
  async_.send();
}

void Reactor::CheckReadyToStop() {
  waiting_conns_.remove_if([](const ConnectionPtr& conn) { return conn->context().ReadyToStop(); });
  if (waiting_conns_.empty()) {
    loop_.break_loop(); // break the epoll loop and terminate the thread
  }
}

// Handle async events.  These events are sent to the reactor by other
// threads that want to bring something to our attention, like the fact that
// we're shutting down, or the fact that there is a new outbound Transfer
// ready to send.
void Reactor::AsyncHandler(ev::async &watcher, int revents) {
  DCHECK(IsCurrentThread());

  BOOST_SCOPE_EXIT(&async_handler_tasks_) {
    async_handler_tasks_.clear();
  } BOOST_SCOPE_EXIT_END;
  if (PREDICT_FALSE(!DrainTaskQueue(&async_handler_tasks_))) {
    ShutdownInternal();
    CheckReadyToStop();
    return;
  }

  for (const auto &task : async_handler_tasks_) {
    task->Run(this);
  }
}

void Reactor::RegisterConnection(const ConnectionPtr& conn) {
  DCHECK(IsCurrentThread());

  // Set a limit on how long the server will negotiate with a new client.
  MonoTime deadline = MonoTime::Now(MonoTime::FINE);
  deadline.AddDelta(MonoDelta::FromMilliseconds(FLAGS_rpc_negotiation_timeout_ms));

  Status s = StartConnectionNegotiation(conn, deadline);
  if (!s.ok()) {
    LOG(ERROR) << "Server connection negotiation failed: " << s.ToString();
    DestroyConnection(conn.get(), s);
  }
  server_conns_.push_back(conn);
}

ConnectionPtr Reactor::AssignOutboundCall(const OutboundCallPtr& call) {
  DCHECK(IsCurrentThread());
  ConnectionPtr conn;

  // TODO: Move call deadline timeout computation into OutboundCall constructor.
  const MonoDelta &timeout = call->controller()->timeout();
  MonoTime deadline;
  if (!timeout.Initialized()) {
    LOG(WARNING) << "Client call " << call->remote_method().ToString()
                 << " has no timeout set for connection id: "
                 << call->conn_id().ToString();
    deadline = MonoTime::Max();
  } else {
    deadline = MonoTime::Now(MonoTime::FINE);
    deadline.AddDelta(timeout);
  }

  Status s = FindOrStartConnection(call->conn_id(), deadline, &conn);
  if (PREDICT_FALSE(!s.ok())) {
    call->SetFailed(s);
    return ConnectionPtr();
  }

  conn->QueueOutboundCall(call);
  return conn;
}

//
// Handles timer events.  The periodic timer:
//
// 1. updates Reactor::cur_time_
// 2. every tcp_conn_timeo_ seconds, close down connections older than
//    tcp_conn_timeo_ seconds.
//
void Reactor::TimerHandler(ev::timer &watcher, int revents) {
  DCHECK(IsCurrentThread());

  if (EV_ERROR & revents) {
    LOG(WARNING) << "Reactor " << name() << " got an error in "
      "the timer handler.";
    return;
  }

  if (stopping_) {
    CheckReadyToStop();
    return;
  }

  MonoTime now(MonoTime::Now(MonoTime::COARSE));
  VLOG(4) << name() << ": timer tick at " << now.ToString();
  cur_time_ = now;

  ScanIdleConnections();
}

void Reactor::RegisterTimeout(ev::timer *watcher) {
  watcher->set(loop_);
}

void Reactor::ScanIdleConnections() {
  DCHECK(IsCurrentThread());
  // enforce TCP connection timeouts
  auto c = server_conns_.begin();
  auto c_end = server_conns_.end();
  uint64_t timed_out = 0;
  for (; c != c_end; ) {
    const ConnectionPtr& conn = *c;
    if (!conn->Idle()) {
      VLOG(3) << "Connection " << conn->ToString() << " not idle";
      ++c; // TODO: clean up this loop
      continue;
    }

    MonoTime last_activity_time = conn->last_activity_time();
    MonoDelta connection_delta(cur_time_.GetDeltaSince(last_activity_time));
    if (connection_delta.MoreThan(connection_keepalive_time_)) {
      conn->Shutdown(STATUS(NetworkError,
                       StringPrintf("connection timed out after %s",
                                    connection_delta.ToString().c_str())));
      VLOG(1) << "Timing out connection " << conn->ToString() << " - it has been idle for "
              << connection_delta.ToSeconds() << "s (delta: " << connection_delta.ToString()
              << ", current time: " << cur_time_.ToString()
              << ", last activity time: " << last_activity_time.ToString() << ")";
      server_conns_.erase(c++);
      ++timed_out;
    } else {
      ++c;
    }
  }

  // TODO: above only times out on the server side.
  // Clients may want to set their keepalive timeout as well.

  VLOG_IF(1, timed_out > 0) << name() << ": timed out " << timed_out << " TCP connections.";
}

bool Reactor::IsCurrentThread() const {
  return thread_.get() == yb::Thread::current_thread();
}

bool Reactor::closing() const {
  std::lock_guard<simple_spinlock> l(pending_tasks_lock_);
  return closing_;
}

void Reactor::RunThread() {
  ThreadRestrictions::SetWaitAllowed(false);
  ThreadRestrictions::SetIOAllowed(false);
  DVLOG(6) << "Calling Reactor::RunThread()...";
  loop_.run(0);
  VLOG(1) << name() << " thread exiting.";

  // No longer need the messenger. This causes the messenger to
  // get deleted when all the reactors exit.
  messenger_.reset();
}

namespace {

Result<Socket> CreateClientSocket(const Endpoint& remote) {
  int flags = Socket::FLAG_NONBLOCKING;
  if (remote.address().is_v6()) {
    flags |= Socket::FLAG_IPV6;
  }
  Socket socket;
  Status status = socket.Init(flags);
  if (status.ok()) {
    status = socket.SetNoDelay(true);
  }
  LOG_IF(WARNING, !status.ok()) << "failed to create an "
      "outbound connection because a new socket could not "
      "be created: " << status.ToString();
  if (!status.ok())
    return status;
  return std::move(socket);
}

} // namespace

Status Reactor::FindOrStartConnection(const ConnectionId &conn_id,
                                      const MonoTime &deadline,
                                      ConnectionPtr* conn) {
  DCHECK(IsCurrentThread());
  conn_map_t::const_iterator c = client_conns_.find(conn_id);
  if (c != client_conns_.end()) {
    *conn = (*c).second;
    return Status::OK();
  }

  // No connection to this remote. Need to create one.
  VLOG(2) << name() << " FindOrStartConnection: creating "
          << "new connection for " << conn_id.remote();

  // Create a new socket and start connecting to the remote.
  auto sock = CreateClientSocket(conn_id.remote());
  RETURN_NOT_OK(sock);
  if (FLAGS_local_ip_for_outbound_sockets.empty()) {
    auto outbound_address = conn_id.remote().address().is_v6()
        ? messenger_->outbound_address_v6()
        : messenger_->outbound_address_v4();
    if (!outbound_address.is_unspecified()) {
      auto status = sock->Bind(Endpoint(outbound_address, 0), /* explain_addr_in_use */ false);
      if (!status.ok()) {
        LOG(WARNING) << "Bind " << outbound_address << " failed: " << status.ToString();
      }
    }
  }
  bool connect_in_progress;
  RETURN_NOT_OK(StartConnect(sock.get_ptr(), conn_id.remote(), &connect_in_progress));

  // Register the new connection in our map.
  *conn = MakeNewConnection(messenger_->connection_type_,
                            this,
                            conn_id.remote(),
                            sock->Release(),
                            ConnectionDirection::CLIENT);
  (*conn)->set_user_credentials(conn_id.user_credentials());

  // Kick off blocking client connection negotiation.
  Status s = StartConnectionNegotiation(*conn, deadline);
  if (s.IsIllegalState()) {
    // Return a nicer error message to the user indicating -- if we just
    // forward the status we'd get something generic like "ThreadPool is closing".
    return STATUS(ServiceUnavailable, "Client RPC Messenger shutting down");
  }
  // Propagate any other errors as-is.
  RETURN_NOT_OK_PREPEND(s, "Unable to start connection negotiation thread");

  // Insert into the client connection map to avoid duplicate connection requests.
  client_conns_.emplace(conn_id, *conn);

  return Status::OK();
}

namespace {

void ShutdownIfRemoteAddressIs(const ConnectionPtr& conn, const IpAddress& address) {
  auto socket = conn->socket();
  Endpoint peer;
  auto status = socket->GetPeerAddress(&peer);
  if (!status.ok()) {
    LOG(WARNING) << "Failed to get peer address" << socket->GetFd() << ": " << status.ToString();
    return;
  }

  if (peer.address() != address) {
    return;
  }

  status = socket->Shutdown(/* shut_read */ true, /* shut_write */ true);
  if (!status.ok()) {
    LOG(WARNING) << "Failed to shutdown " << socket->GetFd() << ": " << status.ToString();
    return;
  }
  LOG(INFO) << "Dropped connection: " << conn->ToString();
}

} // namespace

void Reactor::DropWithRemoteAddress(const IpAddress& address) {
  DCHECK(IsCurrentThread());

  for (auto& conn : server_conns_) {
    ShutdownIfRemoteAddressIs(conn, address);
  }

  for (auto& pair : client_conns_) {
    ShutdownIfRemoteAddressIs(pair.second, address);
  }
}

Status Reactor::StartConnectionNegotiation(const ConnectionPtr& conn,
                                           const MonoTime& deadline) {
  DCHECK(IsCurrentThread());

  scoped_refptr<Trace> trace(new Trace());
  ADOPT_TRACE(trace.get());
  TRACE("Submitting negotiation task for $0", conn->ToString());
  RETURN_NOT_OK(messenger_->negotiation_pool()->SubmitClosure(
      Bind(&Negotiation::RunNegotiation, conn, deadline)));
  return Status::OK();
}

void Reactor::CompleteConnectionNegotiation(const ConnectionPtr& conn,
      const Status &status) {
  DCHECK(IsCurrentThread());
  if (PREDICT_FALSE(!status.ok())) {
    DestroyConnection(conn.get(), status);
    return;
  }

  // Switch the socket back to non-blocking mode after negotiation.
  Status s = conn->SetNonBlocking(true);
  if (PREDICT_FALSE(!s.ok())) {
    LOG(DFATAL) << "Unable to set connection to non-blocking mode: " << s.ToString();
    DestroyConnection(conn.get(), s);
    return;
  }
  conn->MarkNegotiationComplete();
  conn->EpollRegister(loop_);
}

Status Reactor::StartConnect(Socket *sock, const Endpoint& remote, bool *in_progress) {
  Status ret = sock->Connect(remote);
  if (ret.ok()) {
    VLOG(3) << "StartConnect: connect finished immediately for " << remote;
    *in_progress = false; // connect() finished immediately.
    return ret;
  }

  if (Socket::IsTemporarySocketError(ret)) {
    // The connect operation is in progress.
    *in_progress = true;
    VLOG(3) << "StartConnect: connect in progress for " << remote;
    return Status::OK();
  } else {
    LOG(WARNING) << "Failed to create an outbound connection to " << remote
                 << " because connect failed: " << ret.ToString();
    return ret;
  }
}

void Reactor::DestroyConnection(Connection *conn, const Status &conn_status) {
  DCHECK(IsCurrentThread());

  VLOG(3) << "DestroyConnection(" << conn->ToString() << ", " << conn_status.ToString() << ")";

  ConnectionPtr retained_conn = conn->shared_from_this();
  conn->Shutdown(conn_status);

  // Unlink connection from lists.
  if (conn->direction() == ConnectionDirection::CLIENT) {
    ConnectionId conn_id(conn->remote(), conn->user_credentials());
    bool erased = false;
    for (int idx = 0; idx < FLAGS_num_connections_to_server; idx++) {
      conn_id.set_idx(idx);
      auto it = client_conns_.find(conn_id);
      if (it != client_conns_.end() && it->second.get() == conn) {
        client_conns_.erase(it);
        erased = true;
      }
    }
    if (!erased) {
      LOG(WARNING) << "Looking for " << conn->ToString() << ", "
                   << conn->user_credentials().ToString();
      for (auto &p : client_conns_) {
        LOG(WARNING) << "  Client connection: " << p.first.ToString() << ", "
                     << p.second->ToString();
      }
    }
    CHECK(erased) << "Couldn't find connection for any index to " << conn->ToString();
  } else if (conn->direction() == ConnectionDirection::SERVER) {
    auto it = server_conns_.begin();
    while (it != server_conns_.end()) {
      if ((*it).get() == conn) {
        server_conns_.erase(it);
        break;
      }
      ++it;
    }
  }
}

void Reactor::ProcessOutboundQueue() {
  {
    std::lock_guard<simple_spinlock> lock(outbound_queue_lock_);
    outbound_queue_.swap(processing_outbound_queue_);
  }
  if (processing_outbound_queue_.empty()) {
    return;
  }
  processing_connections_.reserve(processing_outbound_queue_.size());
  for (auto& call : processing_outbound_queue_) {
    auto conn = AssignOutboundCall(call);
    processing_connections_.push_back(std::move(conn));
  }
  processing_outbound_queue_.clear();
  std::sort(processing_connections_.begin(), processing_connections_.end());
  auto new_end = std::unique(processing_connections_.begin(), processing_connections_.end());
  processing_connections_.erase(new_end, processing_connections_.end());
  for (auto& conn : processing_connections_) {
    if (conn) {
      conn->OutboundQueued();
    }
  }
  processing_connections_.clear();
}

void Reactor::QueueOutboundCall(OutboundCallPtr call) {
  DVLOG(3) << "Queueing outbound call "
           << call->ToString() << " to remote " << call->conn_id().remote();

  bool was_empty = false;
  bool closing = false;
  {
    std::lock_guard<simple_spinlock> lock(outbound_queue_lock_);
    if (!outbound_queue_stopped_) {
      was_empty = outbound_queue_.empty();
      outbound_queue_.push_back(call);
    } else {
      closing = true;
    }
  }
  if (closing) {
    call->Transferred(ShutdownError(true));
    return;
  }
  if (was_empty) {
    ScheduleReactorTask(process_outbound_queue_task_);
  }
  TRACE_TO(call->trace(), "Scheduled.");
}

DelayedTask::DelayedTask(std::function<void(const Status&)> func, MonoDelta when, int64_t id,
                         std::shared_ptr<Messenger> messenger)
    : func_(std::move(func)),
      when_(std::move(when)),
      id_(id),
      messenger_(std::move(messenger)) {}

void DelayedTask::Run(Reactor* reactor) {
  DCHECK(reactor_ == nullptr) << "Task has already been scheduled";
  DCHECK(reactor->IsCurrentThread());

  // Aquire lock to prevent task from being aborted in the middle of scheduling, in case abort
  // will be requested in the middle of scheduling - task will be aborted right after return
  // from this method.
  std::lock_guard<LockType> l(lock_);
  if (done_) {
    // Task has been aborted.
    return;
  }

  // Schedule the task to run later.
  reactor_ = reactor;
  timer_.set(reactor->loop_);

  // timer_ is owned by this task and will be stopped through AbortTask/Abort before this task
  // is removed from list of scheduled tasks, so it is safe for timer_ to remember pointer to task.
  timer_.set<DelayedTask, &DelayedTask::TimerHandler>(this);

  timer_.start(when_.ToSeconds(), // after
               0);                // repeat
  reactor_->scheduled_tasks_.insert(shared_from(this));
}

bool DelayedTask::MarkAsDone() {
  std::lock_guard<LockType> l(lock_);
  if (done_) {
    return false;
  } else {
    done_ = true;
    return true;
  }
}

void DelayedTask::AbortTask(const Status& abort_status) {
  if (MarkAsDone()) {
    timer_.stop();
    func_(abort_status);
  }
}

void DelayedTask::Abort(const Status& abort_status) {
  // We are just calling lock-protected AbortTask here to avoid concurrent execution of func_ due
  // to AbortTasks execution from non-reactor threads prior to reactor shutdown.
  AbortTask(abort_status);
}

void DelayedTask::TimerHandler(ev::timer& watcher, int revents) {
  if (!MarkAsDone()) {
    // The task has been already executed by Abort/AbortTask.
    return;
  }
  // Hold shared_ptr, so this task wouldn't be destroyed upon removal below until func_ is called
  auto holder = shared_from_this();

  reactor_->scheduled_tasks_.erase(shared_from(this));
  if (messenger_ != nullptr) {
    messenger_->RemoveScheduledTask(id_);
  }

  if (EV_ERROR & revents) {
    string msg = "Delayed task got an error in its timer handler";
    LOG(WARNING) << msg;
    func_(STATUS(Aborted, msg));
  } else {
    func_(Status::OK());
  }
}

void Reactor::RegisterInboundSocket(Socket *socket, const Endpoint& remote) {
  VLOG(3) << name_ << ": new inbound connection to " << remote;
  auto conn = MakeNewConnection(messenger_->connection_type_,
                                this,
                                remote,
                                socket->Release(),
                                ConnectionDirection::SERVER);
  ScheduleReactorFunctor([conn = std::move(conn)](Reactor* reactor) {
    reactor->RegisterConnection(conn);
  });
}

void Reactor::ScheduleReactorTask(std::shared_ptr<ReactorTask> task) {
  {
    std::unique_lock<simple_spinlock> l(pending_tasks_lock_);
    if (closing_) {
      // We guarantee the reactor lock is not taken when calling Abort().
      l.unlock();
      task->Abort(ShutdownError(false));
      return;
    }
    pending_tasks_.push_back(std::move(task));
  }
  WakeThread();
}

bool Reactor::DrainTaskQueue(std::vector<std::shared_ptr<ReactorTask>>* tasks) {
  CHECK(tasks->empty());
  std::lock_guard<simple_spinlock> l(pending_tasks_lock_);
  tasks->swap(pending_tasks_);
  return !closing_;
}

// Task to call an arbitrary function within the reactor thread.
template<class F>
class RunFunctionTask : public ReactorTask {
 public:
  explicit RunFunctionTask(const F& f) : function_(f), latch_(1) {}

  void Run(Reactor *reactor) override {
    status_ = function_(reactor);
    latch_.CountDown();
  }

  void Abort(const Status &status) override {
    status_ = status;
    latch_.CountDown();
  }

  // Wait until the function has completed, and return the Status
  // returned by the function.
  Status Wait() {
    latch_.Wait();
    return status_;
  }

 private:
  F function_;
  Status status_;
  CountDownLatch latch_;
};

template<class F>
Status Reactor::RunOnReactorThread(const F& f) {
  auto task = std::make_shared<RunFunctionTask<F>>(f);
  ScheduleReactorTask(task);
  return task->Wait();
}

}  // namespace rpc
}  // namespace yb
