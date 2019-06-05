#include <iostream>
#include <fstream>
#include <unistd.h>
#include <vector>
#include <map>
#include <mxnet/sa_util.h>
#include <dmlc/parameter.h>
#include <dmlc/logging.h>
#include "./gpu_odswap.h"
#include "./gpu_swap_history.h"
#include "./gpu_swap_prefetch.h"


namespace mxnet {

Prefetch::Prefetch() {
  num_loop_ = dmlc::GetEnv("MXNET_NUM_LOOP", 10); 
  cur_node_idx_ = cur_idx_in_node_ = 0;
  prefetching_ = false;
  sem_init(&prefetch_sem_, 0, 1);
}

Prefetch::~Prefetch() {}

Prefetch* Prefetch::Get() {
  static Prefetch *s = _GetSharedRef().get();
  return s;
}

std::shared_ptr<Prefetch> Prefetch::_GetSharedRef() {
  static std::shared_ptr<Prefetch> inst(new Prefetch());
  return inst;
}

void Prefetch::StartPrefetching() {
  sa_log << "Prefetch: Start Prefetching" << std::endl;
  prefetching_ = true;
  prefetcher_ = std::thread(&Prefetch::Prefetching, this);
  sa_log << "Prefetch: Start Prefetching, thread created" << std::endl;
}

void Prefetch::StopPrefetching() {
  /* Prefetch stops in the last iteration */
}

void Prefetch::Prefetching() {
  bool success;
  while (true) {
    sa_log << "Prefetch: prefetching " 
           << prefetch_sequence_[cur_node_idx_][cur_idx_in_node_] << std::endl;
    ODSwap::Get()->GetAddr(prefetch_sequence_[cur_node_idx_][cur_idx_in_node_],
        2, success);
    sa_log << "Prefetch: " << (success?"success":"failure") << std::endl;
    if (!success) {
      sem_wait(&prefetch_sem_);
    } else {
      cur_idx_in_node_++;
      if (cur_idx_in_node_ == prefetch_sequence_[cur_node_idx_].size()) {
        sa_log << "Prefetch: End of node with index " << cur_node_idx_ << std::endl;
        cur_idx_in_node_ = 0;
        cur_node_idx_++;
        if (cur_node_idx_ == prefetch_sequence_.size()) {
          sa_log << "Prefetch: End of iteration" << std::endl;
          if (MemoryHistory::Get()->GetIterationIdx() == num_loop_) {
            sa_log << "Prefetch: Reach last node in last iteration" << std::endl;
            break;   
          }
          cur_node_idx_ = 0;
        }
      }
    } // if (!success)
  } // While true
}

void Prefetch::PushHandlesToPrefetch(const std::unordered_set<handle_t>& handles) {
  prefetch_sequence_.push_back(std::vector<handle_t>{});  
  auto& cur_subseq = prefetch_sequence_[prefetch_sequence_.size()-1];
  for (auto handle: handles) {
    cur_subseq.push_back(handle);
  }
}

void Prefetch::SignalContinue() {
  sa_log << "Prefetch: SignalContinue" << std::endl;
  sem_post(&prefetch_sem_);
}

} // namespace mxnet
