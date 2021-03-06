/* -*- indent-tabs-mode: nil -*- */

#include <chrono>
#include <csignal>
#include <cstring>
#include <event2/buffer.h>
#include <event2/thread.h>
#include <functional>
#include <gflags/gflags.h>
#include <iostream>
#include <memory>
#include <mutex>
#include <openssl/crypto.h>
#include <openssl/err.h>
#include <signal.h>
#include <string>
#include <unistd.h>


#include "config.h"
#include "client/async_log_client.h"
#include "fetcher/continuous_fetcher.h"
#include "fetcher/remote_peer.h"
#include "fetcher/peer_group.h"
#include "log/cluster_state_controller.h"
#include "log/ct_extensions.h"
#include "log/etcd_consistent_store.h"
#include "log/file_db.h"
#include "log/file_storage.h"
#include "log/leveldb_db.h"
#include "log/sqlite_db.h"
#include "log/strict_consistent_store.h"
#include "merkletree/merkle_verifier.h"
#include "monitoring/latency.h"
#include "monitoring/monitoring.h"
#include "monitoring/registry.h"
#include "server/handler.h"
#include "server/json_output.h"
#include "server/metrics.h"
#include "server/proxy.h"
#include "server/server.h"
#include "util/etcd.h"
#include "util/fake_etcd.h"
#include "util/libevent_wrapper.h"
#include "util/masterelection.h"
#include "util/periodic_closure.h"
#include "util/read_key.h"
#include "util/status.h"
#include "util/thread_pool.h"
#include "util/uuid.h"

DEFINE_string(server, "localhost", "Server host");
DEFINE_int32(port, 9999, "Server port");
// TODO(alcutter): Just specify a root dir with a single flag.
DEFINE_string(cert_dir, "", "Storage directory for certificates");
DEFINE_string(tree_dir, "", "Storage directory for trees");
DEFINE_string(meta_dir, "", "Storage directory for meta info");
DEFINE_string(sqlite_db, "",
              "SQLite database for certificate and tree storage");
DEFINE_string(leveldb_db, "",
              "LevelDB database for certificate and tree storage");
// TODO(ekasper): sanity-check these against the directory structure.
DEFINE_int32(cert_storage_depth, 0,
             "Subdirectory depth for certificates; if the directory is not "
             "empty, must match the existing depth.");
DEFINE_int32(tree_storage_depth, 0,
             "Subdirectory depth for tree signatures; if the directory is not "
             "empty, must match the existing depth");
DEFINE_int32(log_stats_frequency_seconds, 3600,
             "Interval for logging summary statistics. Approximate: the "
             "server will log statistics if in the beginning of its select "
             "loop, at least this period has elapsed since the last log time. "
             "Must be greater than 0.");
DEFINE_int32(target_poll_frequency_seconds, 10,
             "How often should the target log be polled for updates.");
DEFINE_string(etcd_host, "", "Hostname of the etcd server");
DEFINE_int32(etcd_port, 0, "Port of the etcd server.");
DEFINE_string(etcd_root, "/root", "Root of cluster entries in etcd.");
DEFINE_int32(num_http_server_threads, 16,
             "Number of threads for servicing the incoming HTTP requests.");
DEFINE_string(target_log_uri, "http://ct.googleapis.com/pilot",
              "URI of the log to mirror.");
DEFINE_string(
    target_public_key, "",
    "PEM-encoded server public key file of the log we're mirroring.");
DEFINE_int32(local_sth_update_frequency_seconds, 30,
             "Number of seconds between local checks for updated tree data.");

namespace libevent = cert_trans::libevent;

using cert_trans::AsyncLogClient;
using cert_trans::ClusterStateController;
using cert_trans::ConsistentStore;
using cert_trans::ContinuousFetcher;
using cert_trans::Counter;
using cert_trans::Gauge;
using cert_trans::EtcdClient;
using cert_trans::EtcdConsistentStore;
using cert_trans::FakeEtcdClient;
using cert_trans::FileStorage;
using cert_trans::HttpHandler;
using cert_trans::JsonOutput;
using cert_trans::Latency;
using cert_trans::LoggedCertificate;
using cert_trans::MasterElection;
using cert_trans::PeriodicClosure;
using cert_trans::Proxy;
using cert_trans::ReadPublicKey;
using cert_trans::RemotePeer;
using cert_trans::Server;
using cert_trans::ScopedLatency;
using cert_trans::StrictConsistentStore;
using cert_trans::ThreadPool;
using cert_trans::Update;
using cert_trans::UrlFetcher;
using ct::ClusterNodeState;
using ct::SignedTreeHead;
using google::RegisterFlagValidator;
using std::bind;
using std::chrono::duration;
using std::chrono::duration_cast;
using std::chrono::milliseconds;
using std::chrono::seconds;
using std::chrono::steady_clock;
using std::function;
using std::lock_guard;
using std::make_pair;
using std::make_shared;
using std::map;
using std::mutex;
using std::placeholders::_1;
using std::shared_ptr;
using std::string;
using std::thread;
using std::unique_ptr;
using util::StatusOr;
using util::SyncTask;
using util::Task;


namespace {


Gauge<>* latest_local_tree_size_gauge =
    Gauge<>::New("latest_local_tree_size",
                 "Size of latest locally available STH.");


// Basic sanity checks on flag values.
static bool ValidatePort(const char* flagname, int port) {
  if (port <= 0 || port > 65535) {
    std::cout << "Port value " << port << " is invalid. " << std::endl;
    return false;
  }
  return true;
}

static const bool port_dummy =
    RegisterFlagValidator(&FLAGS_port, &ValidatePort);

static bool ValidateRead(const char* flagname, const string& path) {
  if (access(path.c_str(), R_OK) != 0) {
    std::cout << "Cannot access " << flagname << " at " << path << std::endl;
    return false;
  }
  return true;
}

static bool ValidateWrite(const char* flagname, const string& path) {
  if (path != "" && access(path.c_str(), W_OK) != 0) {
    std::cout << "Cannot modify " << flagname << " at " << path << std::endl;
    return false;
  }
  return true;
}

static const bool pubkey_dummy =
    RegisterFlagValidator(&FLAGS_target_public_key, &ValidateRead);

static const bool cert_dir_dummy =
    RegisterFlagValidator(&FLAGS_cert_dir, &ValidateWrite);

static const bool tree_dir_dummy =
    RegisterFlagValidator(&FLAGS_tree_dir, &ValidateWrite);

static bool ValidateIsNonNegative(const char* flagname, int value) {
  if (value < 0) {
    std::cout << flagname << " must not be negative" << std::endl;
    return false;
  }
  return true;
}

static const bool c_st_dummy =
    RegisterFlagValidator(&FLAGS_cert_storage_depth, &ValidateIsNonNegative);
static const bool t_st_dummy =
    RegisterFlagValidator(&FLAGS_tree_storage_depth, &ValidateIsNonNegative);

static bool ValidateIsPositive(const char* flagname, int value) {
  if (value <= 0) {
    std::cout << flagname << " must be greater than 0" << std::endl;
    return false;
  }
  return true;
}

static const bool stats_dummy =
    RegisterFlagValidator(&FLAGS_log_stats_frequency_seconds,
                          &ValidateIsPositive);

static const bool follow_dummy =
    RegisterFlagValidator(&FLAGS_target_poll_frequency_seconds,
                          &ValidateIsPositive);
}  // namespace


void STHUpdater(
    Database<LoggedCertificate>* db,
    ClusterStateController<LoggedCertificate>* cluster_state_controller,
    mutex* queue_mutex, map<int64_t, ct::SignedTreeHead>* queue, Task* task) {
  CHECK_NOTNULL(db);
  CHECK_NOTNULL(cluster_state_controller);
  CHECK_NOTNULL(queue_mutex);
  CHECK_NOTNULL(queue);
  CHECK_NOTNULL(task);

  while (true) {
    if (task->CancelRequested()) {
      task->Return(util::Status::CANCELLED);
    }

    const int64_t local_size(db->TreeSize());
    latest_local_tree_size_gauge->Set(local_size);

    {
      lock_guard<mutex> lock(*queue_mutex);
      while (!queue->empty() &&
             queue->begin()->second.tree_size() <= local_size) {
        LOG(INFO) << "Can serve new STH of size "
                  << queue->begin()->second.tree_size() << " locally";
        cluster_state_controller->NewTreeHead(queue->begin()->second);
        queue->erase(queue->begin());
      }
    }

    std::this_thread::sleep_for(
        seconds(FLAGS_local_sth_update_frequency_seconds));
  }
}


int main(int argc, char* argv[]) {
  // Ignore various signals whilst we start up.
  signal(SIGHUP, SIG_IGN);
  signal(SIGINT, SIG_IGN);
  signal(SIGTERM, SIG_IGN);

  google::ParseCommandLineFlags(&argc, &argv, true);
  google::InitGoogleLogging(argv[0]);
  google::InstallFailureSignalHandler();

  Server<LoggedCertificate>::StaticInit();

  if (!FLAGS_sqlite_db.empty() + !FLAGS_leveldb_db.empty() +
          (!FLAGS_cert_dir.empty() | !FLAGS_tree_dir.empty()) !=
      1) {
    std::cerr << "Must only specify one database type.";
    exit(1);
  }

  if (FLAGS_sqlite_db.empty() && FLAGS_leveldb_db.empty()) {
    CHECK_NE(FLAGS_cert_dir, FLAGS_tree_dir)
        << "Certificate directory and tree directory must differ";
  }

  Database<LoggedCertificate>* db;

  if (!FLAGS_sqlite_db.empty()) {
    db = new SQLiteDB<LoggedCertificate>(FLAGS_sqlite_db);
  } else if (!FLAGS_leveldb_db.empty()) {
    db = new LevelDB<LoggedCertificate>(FLAGS_leveldb_db);
  } else {
    db = new FileDB<LoggedCertificate>(
        new FileStorage(FLAGS_cert_dir, FLAGS_cert_storage_depth),
        new FileStorage(FLAGS_tree_dir, FLAGS_tree_storage_depth),
        new FileStorage(FLAGS_meta_dir, 0));
  }

  const bool stand_alone_mode(FLAGS_etcd_host.empty());
  const shared_ptr<libevent::Base> event_base(make_shared<libevent::Base>());
  UrlFetcher url_fetcher(event_base.get());

  const std::unique_ptr<EtcdClient> etcd_client(
      stand_alone_mode
          ? new FakeEtcdClient(event_base.get())
          : new EtcdClient(&url_fetcher, FLAGS_etcd_host, FLAGS_etcd_port));

  Server<LoggedCertificate>::Options options;
  options.server = FLAGS_server;
  options.port = FLAGS_port;
  options.etcd_root = FLAGS_etcd_root;
  options.num_http_server_threads = FLAGS_num_http_server_threads;

  Server<LoggedCertificate> server(options, event_base, db, etcd_client.get(),
                                   &url_fetcher, nullptr, nullptr);
  server.Initialise(true /* is_mirror */);

  if (stand_alone_mode) {
    // Set up a simple single-node mirror environment for testing.
    //
    // Put a sensible single-node config into FakeEtcd. For a real clustered
    // log
    // we'd expect a ClusterConfig already to be present within etcd as part of
    // the provisioning of the log.
    //
    // TODO(alcutter): Note that we're currently broken wrt to restarting the
    // log server when there's data in the log.  It's a temporary thing though,
    // so fear ye not.
    ct::ClusterConfig config;
    config.set_minimum_serving_nodes(1);
    config.set_minimum_serving_fraction(1);
    LOG(INFO) << "Setting default single-node ClusterConfig:\n"
              << config.DebugString();
    server.consistent_store()->SetClusterConfig(config);

    // Since we're a single node cluster, we'll settle that we're the
    // master here, so that we can populate the initial STH
    // (StrictConsistentStore won't allow us to do so unless we're master.)
    server.election()->StartElection();
    server.election()->WaitToBecomeMaster();
  } else {
    CHECK(!FLAGS_server.empty());
  }

  CHECK(!FLAGS_target_public_key.empty());
  CHECK(!FLAGS_target_log_uri.empty());

  const StatusOr<EVP_PKEY*> pubkey(ReadPublicKey(FLAGS_target_public_key));
  CHECK(pubkey.ok()) << "Failed to read target log's public key file: "
                     << pubkey.status();

  ThreadPool pool(16);
  SyncTask fetcher_task(&pool);

  mutex queue_mutex;
  map<int64_t, ct::SignedTreeHead> queue;

  const function<void(const ct::SignedTreeHead&)> new_sth(
      [&queue_mutex, &queue](const ct::SignedTreeHead& sth) {
        lock_guard<mutex> lock(queue_mutex);
        const auto it(queue.find(sth.tree_size()));
        if (it != queue.end() && sth.timestamp() < it->second.timestamp()) {
          LOG(WARNING) << "Received older STH:\nHad:\n"
                       << it->second.DebugString() << "\nGot:\n"
                       << sth.DebugString();
          return;
        }
        queue.insert(make_pair(sth.tree_size(), sth));
      });

  const shared_ptr<RemotePeer> peer(make_shared<RemotePeer>(
      unique_ptr<AsyncLogClient>(
          new AsyncLogClient(&pool, &url_fetcher, FLAGS_target_log_uri)),
      unique_ptr<LogVerifier>(
          new LogVerifier(new LogSigVerifier(pubkey.ValueOrDie()),
                          new MerkleVerifier(new Sha256Hasher))),
      new_sth, fetcher_task.task()->AddChild(
                   [](Task* task) { LOG(INFO) << "RemotePeer exited."; })));
  const unique_ptr<ContinuousFetcher> fetcher(
      ContinuousFetcher::New(event_base.get(), &pool, db, false));
  fetcher->AddPeer("target", peer);

  thread sth_updater(&STHUpdater, db, server.cluster_state_controller(),
                     &queue_mutex, &queue,
                     fetcher_task.task()->AddChild([](Task* task) {
                       LOG(INFO) << "STHUpdater exited.";
                     }));

  server.Run();

  fetcher_task.task()->Return();
  fetcher_task.Wait();
  sth_updater.join();

  return 0;
}
