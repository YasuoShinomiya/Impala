// Copyright 2014 Cloudera Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "scheduling/admission-controller.h"

#include <boost/algorithm/string.hpp>
#include <boost/bind.hpp>
#include <boost/foreach.hpp>
#include <boost/mem_fn.hpp>
#include <gutil/strings/substitute.h>

#include "common/logging.h"
#include "statestore/query-schedule.h"
#include "statestore/simple-scheduler.h"
#include "runtime/exec-env.h"
#include "runtime/mem-tracker.h"
#include "util/debug-util.h"
#include "util/time.h"
#include "util/runtime-profile.h"

using namespace std;
using namespace boost;
using namespace strings;

// TODO: Remove flags once the configuration mechanisms are in place
DEFINE_string(default_pool_name, "default-pool", "Default pool name.");
DEFINE_int64(default_pool_max_requests, -1, "Maximum number of concurrent outstanding "
    "requests allowed to run before queueing incoming requests. A negative value "
    "indicates no limit.");
DEFINE_int64(default_pool_max_mem_usage, -1, "Maximum amount of memory usage (in bytes) "
    "that all outstanding requests in this pool may use before new requests to this pool"
    " are queued. A negative value indicates no memory limit.");
DEFINE_int64(default_pool_max_queue_size, 0, "Maximum number of requests allowed to be "
    "queued before rejecting requests. A negative value or 0 indicates requests "
    "will always be rejected once the maximum number of concurrent requests are "
    "executing.");
DEFINE_int64(default_queue_wait_timeout_ms, 60 * 1000, "Maximum amount of time (in "
    "milliseconds) that a request will wait to be admitted before timing out.");

namespace impala {

const string AdmissionController::IMPALA_REQUEST_QUEUE_TOPIC("impala-request-queue");

// Delimiter used for topic keys of the form "<pool_name><delimiter><backend_id>".
// "!" is used because the backend id contains a colon, but it should not contain "!".
// When parsing the topic key we need to be careful to find the last instance in
// case the pool name contains it as well.
const char TOPIC_KEY_DELIMITER = '!';

// Define metric key format strings for metrics in PoolMetrics
// '$0' is replaced with the pool name by strings::Substitute
const string LOCAL_ADMITTED_METRIC_KEY_FORMAT =
  "admission-controller.$0.local-admitted";
const string LOCAL_QUEUED_METRIC_KEY_FORMAT =
  "admission-controller.$0.local-queued";
const string LOCAL_DEQUEUED_METRIC_KEY_FORMAT =
  "admission-controller.$0.local-dequeued";
const string LOCAL_REJECTED_METRIC_KEY_FORMAT =
  "admission-controller.$0.local-rejected";
const string LOCAL_TIMED_OUT_METRIC_KEY_FORMAT =
  "admission-controller.$0.local-timed-out";
const string LOCAL_COMPLETED_METRIC_KEY_FORMAT =
  "admission-controller.$0.local-completed";
const string LOCAL_TIME_IN_QUEUE_METRIC_KEY_FORMAT =
  "admission-controller.$0.local-time-in-queue-ms";
const string CLUSTER_NUM_RUNNING_METRIC_KEY_FORMAT =
  "admission-controller.$0.cluster-num-running";
const string CLUSTER_IN_QUEUE_METRIC_KEY_FORMAT =
  "admission-controller.$0.cluster-in-queue";
const string CLUSTER_MEM_USAGE_METRIC_KEY_FORMAT =
  "admission-controller.$0.cluster-mem-usage";
const string LOCAL_NUM_RUNNING_METRIC_KEY_FORMAT =
  "admission-controller.$0.local-num-running";
const string LOCAL_IN_QUEUE_METRIC_KEY_FORMAT =
  "admission-controller.$0.local-in-queue";
const string LOCAL_MEM_USAGE_METRIC_KEY_FORMAT =
  "admission-controller.$0.local-mem-usage";

// Profile query events
const string QUERY_EVENT_SUBMIT_FOR_ADMISSION = "Submit for admission";
const string QUERY_EVENT_COMPLETED_ADMISSION = "Completed admission";

// Profile info string for admission result
const string PROFILE_INFO_KEY_ADMISSION_RESULT = "Admission result";
const string PROFILE_INFO_VAL_ADMIT_IMMEDIATELY = "Admitted immediately";
const string PROFILE_INFO_VAL_ADMIT_QUEUED = "Admitted (queued)";
const string PROFILE_INFO_VAL_REJECTED = "Rejected";
const string PROFILE_INFO_VAL_TIME_OUT = "Timed out (queued)";

// Error status string formats
// $0 = query_id
const string STATUS_REJECTED = "Rejected query id=$0";
// $0 = query_id, $1 = timeout in milliseconds
const string STATUS_TIME_OUT = "Admission for query id=$0 exceeded timeout $1ms";

// Returns the pool name from the topic key if it is valid.
// 'is_valid' returns whether the topic key is valid.
static inline string ExtractPoolName(const string& topic_key, bool* is_valid) {
  *is_valid = true;
  size_t pos = topic_key.find_last_of(TOPIC_KEY_DELIMITER);
  if (pos == string::npos || pos >= topic_key.size() - 1) {
    VLOG_QUERY << "Invalid topic key for pool: " << topic_key;
    *is_valid = false;
    return topic_key;
  }
  return topic_key.substr(0, pos);
}

// Returns the topic key for the pool at this backend, i.e. a string of the
// form: "<pool_name><delimiter><backend_id>".
static inline string MakePoolTopicKey(const string& pool_name,
    const string& backend_id) {
  // Ensure the backend_id does not contain the delimiter to ensure that the topic key
  // can be parsed properly by finding the last instance of the delimiter.
  DCHECK_EQ(backend_id.find(TOPIC_KEY_DELIMITER), string::npos);
  return Substitute("$0$1$2", pool_name, TOPIC_KEY_DELIMITER, backend_id);
}

// Returns a debug string for the given local and total pool stats. Either
// 'total_stats' or 'local_stats' may be NULL to skip writing those stats.
static string DebugPoolStats(const string& pool_name,
    const TPoolStats* total_stats,
    const TPoolStats* local_stats) {
  stringstream ss;
  ss << "pool=" << pool_name;
  if (total_stats != NULL) {
    ss << " Total(";
    ss << "num_running=" << total_stats->num_running << ", ";
    ss << "num_queued=" << total_stats->num_queued << ", ";
    ss << "mem_usage=" <<
        PrettyPrinter::Print(total_stats->mem_usage, TCounterType::BYTES);
    ss << ")";
  }
  if (local_stats != NULL) {
    ss << " Local(";
    ss << "num_running=" << local_stats->num_running << ", ";
    ss << "num_queued=" << local_stats->num_queued << ", ";
    ss << "mem_usage=" <<
        PrettyPrinter::Print(local_stats->mem_usage, TCounterType::BYTES);
    ss << ")";
  }
  return ss.str();
}

// Computes the number of requests that can be admitted based on the total and local
// pool statistics, pool limits, and whether or not the requests to be admitted are
// from the queue (versus a new request in AdmitQuery()).
static int64_t NumToAdmit(TPoolStats* total_stats, TPoolStats* local_stats,
    int64_t max_requests, int64_t max_mem_usage, bool admit_from_queue) {
  // Can't admit any requests if:
  //  (a) Already over the maximum number of requests
  //  (b) Already over the maximum memory usage
  //  (c) This is not admitting from the queue and there are already queued requests
  if ((max_requests > 0 && total_stats->num_running >= max_requests) ||
      (max_mem_usage > 0 && total_stats->mem_usage >= max_mem_usage) ||
      (!admit_from_queue && total_stats->num_queued > 0)) {
    return 0L;
  }

  // Admit if there is no limit on the number of requests. We don't attempt to estimate
  // memory usage of a request.
  if (max_requests < 0) {
    if (admit_from_queue) {
      DCHECK_GT(local_stats->num_queued, 0);
      // TODO: Consider throttling the number to admit, though if these queries were
      // all submitted at the same time while there is memory available, then they
      // would all be admitted immediately, so this behavior isn't inconsistent with
      // the behavior if admit_from_queue is false.
      return local_stats->num_queued;
    } else {
      DCHECK_EQ(local_stats->num_queued, 0);
      return 1L;
    }
  }

  const int64_t total_available = max_requests - total_stats->num_running;
  DCHECK_GT(total_available, 0);
  if (!admit_from_queue) return 1L;
  double queue_size_ratio = static_cast<double>(local_stats->num_queued) /
    static_cast<double>(total_stats->num_queued);
  // TODO: Use a simple heuristic rather than always admitting at least 1 query.
  return max(1L, static_cast<int64_t>(queue_size_ratio * total_available));
}

AdmissionController::AdmissionController(Metrics* metrics, const string& backend_id)
    : metrics_(metrics),
      backend_id_(backend_id),
      thrift_serializer_(false),
      done_(false) {
  dequeue_thread_.reset(new Thread("scheduling", "admission-thread",
        &AdmissionController::DequeueLoop, this));
}

AdmissionController::~AdmissionController() {
  // The AdmissionController should live for the lifetime of the impalad, but
  // for unit tests we need to ensure that no thread is waiting on the
  // condition variable. This notifies the dequeue thread to stop and waits
  // for it to finish.
  {
    // Lock to ensure the dequeue thread will see the update to done_
    lock_guard<mutex> l(admission_ctrl_lock_);
    done_ = true;
    dequeue_cv_.notify_one();
  }
  dequeue_thread_->Join();
}

Status AdmissionController::Init(StatestoreSubscriber* subscriber) {
  StatestoreSubscriber::UpdateCallback cb =
    bind<void>(mem_fn(&AdmissionController::UpdatePoolStats), this, _1, _2);
  Status status = subscriber->AddTopic(IMPALA_REQUEST_QUEUE_TOPIC, true, cb);
  if (!status.ok()) {
    status.AddErrorMsg("AdmissionController failed to register request queue topic");
  }
  return status;
}

Status AdmissionController::AdmitQuery(QuerySchedule* schedule) {
  // TODO: Get the pool from the schedule and configs from the configuration
  const int64_t max_requests = FLAGS_default_pool_max_requests;
  const int64_t max_queued = FLAGS_default_pool_max_queue_size;
  const int64_t max_mem_usage = FLAGS_default_pool_max_mem_usage;
  const string& pool = FLAGS_default_pool_name;
  schedule->set_request_pool(pool);

  // Note the queue_node will not exist in the queue when this method returns.
  QueueNode queue_node(schedule->query_id());

  schedule->query_events()->MarkEvent(QUERY_EVENT_SUBMIT_FOR_ADMISSION);
  ScopedEvent completedEvent(schedule->query_events(), QUERY_EVENT_COMPLETED_ADMISSION);
  {
    lock_guard<mutex> lock(admission_ctrl_lock_);
    RequestQueue* queue = &request_queue_map_[pool];
    PoolMetrics* pool_metrics = GetPoolMetrics(pool);
    TPoolStats* total_stats = &cluster_pool_stats_[pool];
    TPoolStats* local_stats = &local_pool_stats_[pool];
    VLOG_ROW << "Schedule for id=" << schedule->query_id() << " in pool=" << pool
             << " max_requests=" << max_requests << " max_queued=" << max_queued
             << " Initial stats: " << DebugPoolStats(pool, total_stats, local_stats);

    if (NumToAdmit(total_stats, local_stats, max_requests, max_mem_usage, false) > 0) {
      // Execute immediately
      pools_for_updates_.insert(pool);
      // The local and total stats get incremented together when we queue so if
      // there were any locally queued queries we should not admit immediately.
      DCHECK_EQ(local_stats->num_queued, 0);
      schedule->set_is_admitted(true);
      schedule->summary_profile()->AddInfoString(PROFILE_INFO_KEY_ADMISSION_RESULT,
          PROFILE_INFO_VAL_ADMIT_IMMEDIATELY);
      ++total_stats->num_running;
      ++local_stats->num_running;
      if (pool_metrics != NULL) pool_metrics->local_admitted->Increment(1L);
      VLOG_QUERY << "Admitted query id=" << schedule->query_id();
      VLOG_ROW << "Final: " << DebugPoolStats(pool, total_stats, local_stats);
      return Status::OK;
    } else if (max_requests == 0 || max_mem_usage == 0 ||
        total_stats->num_queued >= max_queued) {
      // Reject query
      schedule->set_is_admitted(false);
      schedule->summary_profile()->AddInfoString(PROFILE_INFO_KEY_ADMISSION_RESULT,
          PROFILE_INFO_VAL_REJECTED);
      if (pool_metrics != NULL) pool_metrics->local_rejected->Increment(1L);
      VLOG_QUERY << "Rejected query id=" << schedule->query_id();
      return Status(Substitute(STATUS_REJECTED, PrintId(schedule->query_id())));
    }

    // We cannot immediately admit but do not need to reject, so queue the request
    VLOG_QUERY << "Queuing, query id=" << schedule->query_id();
    DCHECK_LT(total_stats->num_queued, max_queued);
    DCHECK(max_requests > 0 || max_mem_usage > 0);
    pools_for_updates_.insert(pool);
    ++local_stats->num_queued;
    ++total_stats->num_queued;
    queue->Enqueue(&queue_node);
    if (pool_metrics != NULL) pool_metrics->local_queued->Increment(1L);
  }

  int64_t wait_start_ms = ms_since_epoch();
  int64_t queue_wait_timeout_ms = max(0L, FLAGS_default_queue_wait_timeout_ms);
  // We just call Get() to block until the result is set or it times out. Note that we
  // don't hold the admission_ctrl_lock_ while we wait on this promise so we need to
  // check the state after acquiring the lock in order to avoid any races because it is
  // Set() by the dequeuing thread while holding admission_ctrl_lock_.
  // TODO: handle cancellation
  bool timed_out;
  queue_node.is_admitted.Get(queue_wait_timeout_ms, &timed_out);
  int64_t wait_time_ms = ms_since_epoch() - wait_start_ms;

  // Take the lock in order to check the result of is_admitted as there could be a race
  // with the timeout. If the Get() timed out, then we need to dequeue the request.
  // Otherwise, the request was admitted and we update the number of running queries
  // stats.
  {
    lock_guard<mutex> lock(admission_ctrl_lock_);
    RequestQueue* queue = &request_queue_map_[pool];
    PoolMetrics* pool_metrics = GetPoolMetrics(pool);
    pools_for_updates_.insert(pool);
    if (pool_metrics != NULL) {
      pool_metrics->local_time_in_queue_ms->Increment(wait_time_ms);
    }
    // Now that we have the lock, check again if the query was actually admitted (i.e.
    // if the promise still hasn't been set), in which case we just admit the query.
    timed_out = !queue_node.is_admitted.IsSet();
    TPoolStats* total_stats = &cluster_pool_stats_[pool];
    TPoolStats* local_stats = &local_pool_stats_[pool];
    if (timed_out) {
      queue->Remove(&queue_node);
      queue_node.is_admitted.Set(false);
      schedule->set_is_admitted(false);
      schedule->summary_profile()->AddInfoString(PROFILE_INFO_KEY_ADMISSION_RESULT,
          PROFILE_INFO_VAL_TIME_OUT);
      --local_stats->num_queued;
      --total_stats->num_queued;
      if (pool_metrics != NULL) pool_metrics->local_timed_out->Increment(1L);
      return Status(Substitute(STATUS_TIME_OUT, PrintId(schedule->query_id()),
            queue_wait_timeout_ms));
    }
    // The dequeue thread updates the stats (to avoid a race condition) so we do
    // not change them here.
    DCHECK(queue_node.is_admitted.Get());
    DCHECK(!queue->Contains(&queue_node));
    schedule->set_is_admitted(true);
    schedule->summary_profile()->AddInfoString(PROFILE_INFO_KEY_ADMISSION_RESULT,
        PROFILE_INFO_VAL_ADMIT_QUEUED);
    if (pool_metrics != NULL) pool_metrics->local_admitted->Increment(1L);
    VLOG_QUERY << "Admitted queued query id=" << schedule->query_id();
    VLOG_ROW << "Final: " << DebugPoolStats(pool, total_stats, local_stats);
    return Status::OK;
  }
}

Status AdmissionController::ReleaseQuery(QuerySchedule* schedule) {
  if (!schedule->is_admitted()) return Status::OK; // No-op if query was not admitted
  const string& pool = FLAGS_default_pool_name;
  {
    lock_guard<mutex> lock(admission_ctrl_lock_);
    TPoolStats* total_stats = &cluster_pool_stats_[pool];
    TPoolStats* local_stats = &local_pool_stats_[pool];
    DCHECK_GT(total_stats->num_running, 0);
    DCHECK_GT(local_stats->num_running, 0);
    --total_stats->num_running;
    --local_stats->num_running;
    PoolMetrics* pool_metrics = GetPoolMetrics(pool);
    if (pool_metrics != NULL) pool_metrics->local_completed->Increment(1L);
    pools_for_updates_.insert(pool);
    VLOG_ROW << "Released query id=" << schedule->query_id() << " "
             << DebugPoolStats(pool, total_stats, local_stats);
  }
  dequeue_cv_.notify_one();
  return Status::OK;
}

// Statestore subscriber callback for IMPALA_REQUEST_QUEUE_TOPIC. First, add any local
// pool stats updates. Then, per_backend_pool_stats_map_ is updated with the updated
// stats from any topic deltas that are received and we recompute the cluster-wide
// aggregate stats.
void AdmissionController::UpdatePoolStats(
    const StatestoreSubscriber::TopicDeltaMap& incoming_topic_deltas,
    vector<TTopicDelta>* subscriber_topic_updates) {
  {
    lock_guard<mutex> lock(admission_ctrl_lock_);
    UpdateMemUsage();
    AddPoolUpdates(subscriber_topic_updates);

    StatestoreSubscriber::TopicDeltaMap::const_iterator topic =
        incoming_topic_deltas.find(IMPALA_REQUEST_QUEUE_TOPIC);
    if (topic != incoming_topic_deltas.end()) {
      const TTopicDelta& delta = topic->second;
      // Delta and non-delta updates are handled the same way, except for a full update
      // we first clear the per_backend_pool_stats_map_. We then update the global map
      // and then re-compute the pool stats for any pools that changed.
      if (!delta.is_delta) {
        VLOG_ROW << "Full impala-request-queue stats update";
        per_backend_pool_stats_map_.clear();
      }
      HandleTopicUpdates(delta.topic_entries);
      HandleTopicDeletions(delta.topic_deletions);
    }
    UpdateClusterAggregates();
  }
  dequeue_cv_.notify_one(); // Dequeue and admit queries on the dequeue thread
}

void AdmissionController::HandleTopicUpdates(const vector<TTopicItem>& topic_updates) {
  BOOST_FOREACH(const TTopicItem& item, topic_updates) {
    bool is_valid;
    string pool_name = ExtractPoolName(item.key, &is_valid);
    if (!is_valid) continue;
    string local_topic_key = MakePoolTopicKey(pool_name, backend_id_);
    // The topic entry from this subscriber is handled specially; the stats coming
    // from the statestore are likely already outdated.
    if (item.key == local_topic_key) continue;
    local_pool_stats_[pool_name]; // Create an entry in the local map if it doesn't exist
    TPoolStats pool_update;
    uint32_t len = item.value.size();
    Status status = DeserializeThriftMsg(reinterpret_cast<const uint8_t*>(
          item.value.data()), &len, false, &pool_update);
    if (!status.ok()) {
      VLOG_QUERY << "Error deserializing pool update with key: " << item.key;
      continue;
    }
    PoolStatsMap& pool_map = per_backend_pool_stats_map_[pool_name];

    // Debug logging
    if (pool_map.find(item.key) != pool_map.end()) {
      VLOG_ROW << "Stats update for key=" << item.key << " previous: "
               << DebugPoolStats(pool_name, NULL, &pool_map[item.key]);
    }
    VLOG_ROW << "Stats update for key=" << item.key << " updated: "
             << DebugPoolStats(pool_name, NULL, &pool_update);

    pool_map[item.key] = pool_update;
    DCHECK(per_backend_pool_stats_map_[pool_name][item.key].num_running ==
        pool_update.num_running);
    DCHECK(per_backend_pool_stats_map_[pool_name][item.key].num_queued ==
        pool_update.num_queued);
  }
}

void AdmissionController::HandleTopicDeletions(const vector<string>& topic_deletions) {
  BOOST_FOREACH(const string& topic_key, topic_deletions) {
    bool is_valid;
    string pool_name = ExtractPoolName(topic_key, &is_valid);
    if (!is_valid) continue;
    PoolStatsMap& pool_map = per_backend_pool_stats_map_[pool_name];
    VLOG_ROW << "Deleting stats for key=" << topic_key << " "
             << DebugPoolStats(pool_name, NULL, &pool_map[topic_key]);
    pool_map.erase(topic_key);
    DCHECK(per_backend_pool_stats_map_[pool_name].find(topic_key) ==
           per_backend_pool_stats_map_[pool_name].end());
  }
}

void AdmissionController::UpdateClusterAggregates() {
  BOOST_FOREACH(PoolStatsMap::value_type& entry, local_pool_stats_) {
    const string& pool_name = entry.first;
    TPoolStats& local_stats = entry.second;
    string local_topic_key = MakePoolTopicKey(pool_name, backend_id_);
    PoolStatsMap& pool_map = per_backend_pool_stats_map_[pool_name];
    TPoolStats total_stats;
    BOOST_FOREACH(const PoolStatsMap::value_type& entry, pool_map) {
      // Skip an update from this subscriber as the information may be outdated.
      // The current local_stats will be added below.
      if (entry.first == local_topic_key) continue;
      total_stats.num_running += entry.second.num_running;
      total_stats.num_queued += entry.second.num_queued;
      total_stats.mem_usage += entry.second.mem_usage;
    }
    total_stats.num_running += local_stats.num_running;
    total_stats.num_queued += local_stats.num_queued;
    total_stats.mem_usage += local_stats.mem_usage;

    DCHECK_GE(total_stats.num_running, 0);
    DCHECK_GE(total_stats.num_queued, 0);
    DCHECK_GE(total_stats.mem_usage, 0);
    DCHECK_GE(total_stats.num_running, local_stats.num_running);
    DCHECK_GE(total_stats.num_queued, local_stats.num_queued);
    DCHECK_GE(total_stats.mem_usage, local_stats.mem_usage);

    cluster_pool_stats_[pool_name] = total_stats;
    PoolMetrics* pool_metrics = GetPoolMetrics(pool_name);
    if (pool_metrics != NULL) {
      pool_metrics->cluster_num_running->Update(total_stats.num_running);
      pool_metrics->cluster_in_queue->Update(total_stats.num_queued);
      pool_metrics->cluster_mem_usage->Update(total_stats.mem_usage);
    }

    if (cluster_pool_stats_[pool_name] != total_stats) {
      VLOG_ROW << "Recomputed stats, previous: "
               << DebugPoolStats(pool_name, &cluster_pool_stats_[pool_name], NULL);
      VLOG_ROW << "Recomputed stats, updated: "
               << DebugPoolStats(pool_name, &total_stats, NULL);
    }
  }
}

void AdmissionController::UpdateMemUsage() {
  BOOST_FOREACH(PoolStatsMap::value_type& entry, local_pool_stats_) {
    const string& pool_name = entry.first;
    TPoolStats* stats = &entry.second;
    MemTracker* tracker = MemTracker::GetRequestPoolMemTracker(pool_name, NULL);
    int64_t current_usage = tracker == NULL ? 0L : tracker->consumption();
    if (current_usage != stats->mem_usage) {
      stats->mem_usage = current_usage;
      pools_for_updates_.insert(pool_name);
      PoolMetrics* pool_metrics = GetPoolMetrics(pool_name);
      if (pool_metrics != NULL) {
        pool_metrics->cluster_mem_usage->Update(current_usage);
      }
    }
  }
}

void AdmissionController::AddPoolUpdates(vector<TTopicDelta>* topic_updates) {
  if (pools_for_updates_.empty()) return;
  topic_updates->push_back(TTopicDelta());
  TTopicDelta& topic_delta = topic_updates->back();
  topic_delta.topic_name = IMPALA_REQUEST_QUEUE_TOPIC;
  BOOST_FOREACH(const string& pool_name, pools_for_updates_) {
    DCHECK(local_pool_stats_.find(pool_name) != local_pool_stats_.end());
    TPoolStats& pool_stats = local_pool_stats_[pool_name];
    VLOG_ROW << "Sending topic update " << DebugPoolStats(pool_name, NULL, &pool_stats);
    topic_delta.topic_entries.push_back(TTopicItem());
    TTopicItem& topic_item = topic_delta.topic_entries.back();
    topic_item.key = MakePoolTopicKey(pool_name, backend_id_);
    Status status = thrift_serializer_.Serialize(&pool_stats, &topic_item.value);
    if (!status.ok()) {
      LOG(WARNING) << "Failed to serialize query pool stats: " << status.GetErrorMsg();
      topic_updates->pop_back();
    }
    PoolMetrics* pool_metrics = GetPoolMetrics(pool_name);
    if (pool_metrics != NULL) {
      pool_metrics->local_num_running->Update(pool_stats.num_running);
      pool_metrics->local_in_queue->Update(pool_stats.num_queued);
      pool_metrics->local_mem_usage->Update(pool_stats.mem_usage);
    }
  }
  pools_for_updates_.clear();
}

void AdmissionController::DequeueLoop() {
  while (true) {
    unique_lock<mutex> lock(admission_ctrl_lock_);
    if (done_) break;
    dequeue_cv_.wait(lock);
    BOOST_FOREACH(PoolStatsMap::value_type& entry, local_pool_stats_) {
      const string& pool_name = entry.first;
      TPoolStats* local_stats = &entry.second;
      const int64_t max_requests = FLAGS_default_pool_max_requests;
      const int64_t max_mem_usage = FLAGS_default_pool_max_mem_usage;
      // We should never have queued any requests in pools where either limit is 0 as no
      // requests should ever be admitted or when both limits are less than 0, i.e.
      // unlimited requests can be admitted and should never be queued.
      if (max_requests == 0 || max_mem_usage == 0 ||
          (max_requests < 0 && max_mem_usage < 0)) {
        DCHECK_EQ(local_stats->num_queued, 0);
      }

      if (local_stats->num_queued == 0) continue; // Nothing to dequeue
      DCHECK(max_requests > 0 || max_mem_usage > 0);
      TPoolStats* total_stats = &cluster_pool_stats_[pool_name];

      DCHECK_GT(local_stats->num_queued, 0);
      DCHECK_GE(total_stats->num_queued, local_stats->num_queued);
      int64_t num_to_admit = NumToAdmit(total_stats, local_stats, max_requests,
          max_mem_usage, true);
      if (num_to_admit <= 0) continue;

      RequestQueue& queue = request_queue_map_[pool_name];
      VLOG_ROW << "Dequeue thread can admit " << num_to_admit << " queries from pool "
               << pool_name << " with " << local_stats->num_queued << " queued queries.";

      PoolMetrics* pool_metrics = GetPoolMetrics(pool_name);
      while (num_to_admit > 0 && !queue.empty()) {
        QueueNode* queue_node = queue.Dequeue();
        DCHECK(queue_node != NULL);
        DCHECK(!queue_node->is_admitted.IsSet());
        --local_stats->num_queued;
        --total_stats->num_queued;
        ++local_stats->num_running;
        ++total_stats->num_running;
        if (pool_metrics != NULL) pool_metrics->local_dequeued->Increment(1L);
        VLOG_ROW << "Dequeuing query id=" << queue_node->query_id;
        queue_node->is_admitted.Set(true);
        --num_to_admit;
      }
      pools_for_updates_.insert(pool_name);
    }
  }
}

AdmissionController::PoolMetrics*
AdmissionController::GetPoolMetrics(const string& pool_name) {
  if (metrics_ == NULL) return NULL;
  PoolMetricsMap::iterator it = pool_metrics_map_.find(pool_name);
  if (it != pool_metrics_map_.end()) return &it->second;

  PoolMetrics* pool_metrics = &pool_metrics_map_[pool_name];
  pool_metrics->local_admitted = metrics_->CreateAndRegisterPrimitiveMetric(
      Substitute(LOCAL_ADMITTED_METRIC_KEY_FORMAT, pool_name), 0L);
  pool_metrics->local_queued = metrics_->CreateAndRegisterPrimitiveMetric(
      Substitute(LOCAL_QUEUED_METRIC_KEY_FORMAT, pool_name), 0L);
  pool_metrics->local_dequeued = metrics_->CreateAndRegisterPrimitiveMetric(
      Substitute(LOCAL_DEQUEUED_METRIC_KEY_FORMAT, pool_name), 0L);
  pool_metrics->local_rejected = metrics_->CreateAndRegisterPrimitiveMetric(
      Substitute(LOCAL_REJECTED_METRIC_KEY_FORMAT, pool_name), 0L);
  pool_metrics->local_timed_out = metrics_->CreateAndRegisterPrimitiveMetric(
      Substitute(LOCAL_TIMED_OUT_METRIC_KEY_FORMAT, pool_name), 0L);
  pool_metrics->local_completed = metrics_->CreateAndRegisterPrimitiveMetric(
      Substitute(LOCAL_COMPLETED_METRIC_KEY_FORMAT, pool_name), 0L);
  pool_metrics->local_time_in_queue_ms = metrics_->CreateAndRegisterPrimitiveMetric(
      Substitute(LOCAL_TIME_IN_QUEUE_METRIC_KEY_FORMAT, pool_name), 0L);
  pool_metrics->cluster_num_running = metrics_->CreateAndRegisterPrimitiveMetric(
      Substitute(CLUSTER_NUM_RUNNING_METRIC_KEY_FORMAT, pool_name), 0L);
  pool_metrics->cluster_in_queue = metrics_->CreateAndRegisterPrimitiveMetric(
      Substitute(CLUSTER_IN_QUEUE_METRIC_KEY_FORMAT, pool_name), 0L);
  pool_metrics->cluster_mem_usage = metrics_->RegisterMetric(new Metrics::BytesMetric(
      Substitute(CLUSTER_MEM_USAGE_METRIC_KEY_FORMAT, pool_name), 0L));
  pool_metrics->local_num_running = metrics_->CreateAndRegisterPrimitiveMetric(
      Substitute(LOCAL_NUM_RUNNING_METRIC_KEY_FORMAT, pool_name), 0L);
  pool_metrics->local_in_queue = metrics_->CreateAndRegisterPrimitiveMetric(
      Substitute(LOCAL_IN_QUEUE_METRIC_KEY_FORMAT, pool_name), 0L);
  pool_metrics->local_mem_usage = metrics_->RegisterMetric(new Metrics::BytesMetric(
      Substitute(LOCAL_MEM_USAGE_METRIC_KEY_FORMAT, pool_name), 0L));
  return pool_metrics;
}
}
