#include "inc/fast_channel.h"
#include "build/test.pb.h"
#include "test/utils.h"

#include <thread>
#include <atomic>
#include <chrono>
#include <algorithm>
#include <memory>

const int duration_seconds = 10;
const int max_rpc_cnt = 1000000;
std::string local_ip, server_ip;

// Benchmark-only controller until B4 introduces the production FastController.
class BenchmarkController final : public google::protobuf::RpcController {
 public:
  void Reset() override { error_.clear(); }
  bool Failed() const override { return !error_.empty(); }
  std::string ErrorText() const override { return error_; }
  void SetFailed(const std::string& reason) override { error_ = reason; }
  void StartCancel() override {}
  bool IsCanceled() const override { return false; }
  void NotifyOnCancel(google::protobuf::Closure*) override {}
 private:
  std::string error_;
};

void Test(int req_bytes, int num_threads, int sleep_us = 0)
{
  std::vector<std::unique_ptr<fast::FastChannel>> channels;
  std::vector<std::unique_ptr<EchoService_Stub>> stubs;
  for (int i = 0; i < num_threads; ++i) {
    channels.emplace_back(new fast::FastChannel(server_ip, 1024));
    stubs.emplace_back(new EchoService_Stub(channels.back().get()));
  }

  const std::string req_str(req_bytes, '#');
  std::atomic<bool> stop{false};
  std::vector<std::thread> workers;
  std::vector<std::vector<int64_t>> samples(num_threads);
  std::vector<uint64_t> successes(num_threads, 0), failures(num_threads, 0);
  for (auto& sample : samples) sample.reserve(max_rpc_cnt);

  using Clock = std::chrono::steady_clock;
  const auto start = Clock::now();
  for (int k = 0; k < num_threads; ++k) {
    workers.emplace_back([&, k] {
      TestRequest request;
      request.set_request(req_str);
      while (!stop.load(std::memory_order_relaxed)) {
        TestResponse response;
        BenchmarkController controller;
        const auto begin = Clock::now();
        stubs[k]->Echo(&controller, &request, &response, nullptr);
        const auto end = Clock::now();
        if (controller.Failed() || response.response() != req_str) {
          ++failures[k];
        } else {
          ++successes[k];
          // Bound storage independently of the total successful call count.
          if (samples[k].size() < static_cast<size_t>(max_rpc_cnt)) {
            samples[k].push_back(
                std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count());
          }
        }
        if (sleep_us) usleep(sleep_us);
      }
    });
  }
  std::this_thread::sleep_for(std::chrono::seconds(duration_seconds));
  stop.store(true, std::memory_order_relaxed);
  for (auto& worker : workers) worker.join();
  // Include in-flight call draining, since their results are counted below.
  const double elapsed = std::chrono::duration<double>(Clock::now() - start).count();

  uint64_t ok = 0, failed = 0;
  std::vector<int64_t> latency;
  for (int i = 0; i < num_threads; ++i) {
    ok += successes[i];
    failed += failures[i];
    latency.insert(latency.end(), samples[i].begin(), samples[i].end());
  }
  std::sort(latency.begin(), latency.end());
  std::cout << "threads=" << num_threads << " success=" << ok
            << " failed=" << failed << " elapsed_s=" << elapsed
            << " success_qps=" << (elapsed > 0 ? ok / elapsed : 0)
            << " samples=" << latency.size()
            << " samples_truncated=" << (ok > latency.size());
  if (latency.empty()) {
    std::cout << " p50_us=NA p99_us=NA p999_us=NA";
  } else {
    auto percentile = [&](double p) {
      return latency[static_cast<size_t>((latency.size() - 1) * p)] / 1000.0;
    };
    std::cout << " p50_us=" << percentile(0.5)
              << " p99_us=" << percentile(0.99)
              << " p999_us=" << percentile(0.999);
  }
  std::cout << std::endl;
}

int main(int argc, char *argv[])
{
  if (argc != 2)
  {
    fprintf(stderr, "usage: ./client server_ip\n");
    exit(EXIT_FAILURE);
  }

  GetLocalIp(local_ip);
  server_ip = argv[1];

  int start_bytes = 32, max_bytes = 1024 * 1024, sleep_us = 0;
  std::vector<int> thread_num_vec = {1, 2, 4, 8, 16, 32, 48};
  while (start_bytes <= max_bytes)
  {
    std::cout << "message size: " << start_bytes << std::endl;
    for (auto th_num : thread_num_vec)
    {
      Test(start_bytes, th_num, sleep_us);
    }
    start_bytes *= 2;
    std::cout << std::endl;
  }

  return 0;
}