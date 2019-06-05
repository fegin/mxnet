#ifndef MXNET_STORAGE_ON_DEMAND_MM_DPTR_H_
#define MXNET_STORAGE_ON_DEMAND_MM_DPTR_H_

#include <mxnet/base.h>
#include <mxnet/storage.h>
#include <mxnet/sa_util.h>
#include <algorithm>
#include <map>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include "./gpu_odswap.h"
#include "./gpu_swap_prefetch.h"

namespace mxnet {
namespace storage {

class OD_MM_Dptr : virtual public MM_Dptr {
 public:
  OD_MM_Dptr() {
    float temp_ratio = dmlc::GetEnv("MXNET_GPU_TEMP_RATIO", kGPUTempRatio);
    temp_size_ = (size_t)(temp_ratio * 1024 * 1024 * 1024);
    cudaError_t e = cudaMalloc(&temp_memory_, temp_size_);
    if (e != cudaSuccess && e != cudaErrorCudartUnloading) {
      LOG(FATAL) << "cudaMalloc failed: " << cudaGetErrorString(e);
    }
    std::cout << "OD_MM_Dptr: initialize fake memory of " << temp_size_ << std::endl;
    odswap_ = ODSwap::_GetSharedRef();
    memory_manager_ = GetMemoryManagerRef();
    memory_history_ = MemoryHistory::_GetSharedRef();
    prefetch_ = Prefetch::_GetSharedRef();
    device_id_ = 0;
    
  }

  ~OD_MM_Dptr() {
    cudaError_t e =  cudaFree(temp_memory_);
    if (e != cudaSuccess && e != cudaErrorCudartUnloading) {
      LOG(FATAL) << "cudaMalloc failed: " << cudaGetErrorString(e);
    }
    //memory_manager_->Free(fake_memory_, 0);
  }

  void* Alloc(handle_t id, size_t size, void* ptr = nullptr) {
    sa_log << "Alloc " << id << " " << size << std::endl;
    size_t iteration_idx = memory_history_->GetIterationIdx();
    if(iteration_idx == 0) { 
      ptr = (void*)id;
      dptr_size_[ptr] = size;
      unalloced_dptrs_.insert(ptr);
      SetDptr(id, ptr, device_id_);
    } else if (iteration_idx == 1) {
      CHECK(size <= temp_size_) << "Temporary Memory too small. Has: "
        << temp_size_ << " Required: " << size << std::endl;
      ptr = temp_memory_;
      temp_handles_.insert(id);
    } else {
      LOG(FATAL) << "Alloc after iteration 1 and not temporary: " 
      << (int)(temp_handles_.find(id) != temp_handles_.end()) << std::endl; 
    }
    return ptr;
  }

  void* Free(handle_t id) override {
    sa_log << "Free " << id << std::endl;
    if(temp_handles_.find(id) != temp_handles_.end()) {
      return nullptr;
    }
    CHECK(dptr_mapping_.find(id) != dptr_mapping_.end())
      << "Nonexistent id: " << id;
    auto it = dptr_mapping_.find(id);
    void* ptr = it->second;
    dptr_mapping_.erase(it);
    odswap_->DelAddr(id);
    return ptr;
  }

  void Release (handle_t id, void* ptr) override {}

  void StartAllocArgs () override { }

  void StopAllocArgs () override { }

  void StartBinding () override { 
    sa_log << "Start Binding" << std::endl;
    fake_memory_ = nullptr;
    size_t avail, total;
    const size_t delta = 1000000000;
    memory_manager_->MemGetInfo(0, &total, &avail);
    sa_log << "Bind: Has memory total = " << total << " avail:" << avail << std::endl;
    while (!memory_manager_->TryAllocate(0, avail)) {
      avail -= delta;
    }
    cudaError_t e  = memory_manager_->Malloc(fake_memory_, avail, 0);
    if (e != cudaSuccess && e != cudaErrorCudartUnloading) {
      LOG(FATAL) << "cudaMalloc failed: " << cudaGetErrorString(e);
    }
    sa_log << "Bind: Allocated fake memory of size: " << avail - delta << std::endl;
    memory_history_->StartPreparation();
  }

  void StopBinding () override { 
    sa_log << "End Binding" << std::endl;
    memory_history_->EndPreparation();
  }

  void StartIteration () override {
    sa_log << "Start iteration" << std::endl;
    cur_nid_idx_ = 0;
    memory_history_->StartIteration();
  }

  void StopIteration () override {
    sa_log << "Stop iteration" << std::endl;
    size_t iteration_idx = memory_history_->GetIterationIdx();
    if (iteration_idx == 1) {
      sa_log << "Fake Memory is freed" << std::endl;
      memory_manager_->Free(fake_memory_, 0);
      sa_log << "Recorded node history size = " << node_history_.size() << std::endl;
    }
    memory_history_->StopIteration();
  }

  void Statistics () override { memory_history_->Statistics(); }

  void RegisterEntry (node_t nid, uint32_t idx, handle_t hid,
                      node_t old_nid, uint32_t old_idx, handle_t old_hid,
                      size_t hdl_size, bool is_var, bool is_swap) override { }

  void NotifyBegin (node_t nid, const std::string& name) override {
    size_t iteration_idx = memory_history_->GetIterationIdx();
    cur_node_ = make_pair(nid, name);
    if (iteration_idx == 1) {
      node_history_.push_back(make_pair(nid, name));
    }
    if (iteration_idx == 2) {
      odswap_->StartComputing(node_handles_[cur_node_]);
    }
    if (iteration_idx >= 2) {
      sa_log << "current nid index = " << cur_nid_idx_
             << " which history node is " << node_history_[cur_nid_idx_].first
             << ": " << node_history_[cur_nid_idx_].second << std::endl;
    }
  }

  void NotifyDone (node_t nid) override {
    size_t iteration_idx = memory_history_->GetIterationIdx();
    if (iteration_idx == 1) {
      sa_log << "NotifyDone: Push handles of " << nid << " to prefetch sequence"
             << std::endl;
      prefetch_->PushHandlesToPrefetch(node_handles_[cur_node_]);
    }
    if (iteration_idx >= 2) {
      CHECK(cur_nid_idx_ < node_history_.size());
      sa_log << "mmdptr: inserting swappable handles for " << cur_node_.first 
             << ": " << cur_node_.second << " with size " 
             << node_handles_[cur_node_].size() << std::endl;
      odswap_->StopComputing(node_handles_[cur_node_]);
      if (iteration_idx >= 3) {
        prefetch_->SignalContinue();
      } else if (cur_nid_idx_ == node_history_.size()-2) {
        sa_log << "Iteration 2: Start Prefetching" << std::endl;
        prefetch_->StartPrefetching();
      }
    }
    cur_nid_idx_++;
  }

  void Finish() override { }

#if 0
  std::vector<uint32_t> GetScheduleDeps(uint32_t nid) override {
    return std::vector<uint32_t>();
  }
#endif

  void* GetDptr (handle_t id) override {
    sa_log << "GetDptr " << id << std::endl;
    if(temp_handles_.find(id) != temp_handles_.end()) {
      sa_log << "GetDptr: " << id << " is temporary" << std::endl;
      return temp_memory_;
    } 
    size_t iteration_idx = memory_history_->GetIterationIdx();
    void* ptr = dptr_mapping_[id]; 
    if (iteration_idx == 0) { // Preparation Stage, always fake
      return fake_memory_;
    } else if (iteration_idx == 1) { // Iteration 1, record history of nodes and handles.
      CHECK(dptr_dev_id_.find(id) != dptr_dev_id_.end())
       << id << " is not setdptred by mm_dptr!";
      CHECK(dptr_dev_id_[id] != -1) << id << " is Alloced for CPU!";
      size_t ptr_size = dptr_size_[ptr];
      auto pair = node_handles_.emplace(cur_node_, std::unordered_set<handle_t>{});
      pair.first->second.insert(id); 
      memory_history_->PutRecord(id, 0, MemoryHistory::GET_ADDR, ptr_size);
      return fake_memory_;
    }
    // Iteration 2, do allocation for each handle. (No Prefetch)
    else if (iteration_idx == 2) {
      if (unalloced_dptrs_.find(ptr) != unalloced_dptrs_.end()) {
        CHECK(dptr_dev_id_.find(id) != dptr_dev_id_.end())
         << id << " is not setdptred by mm_dptr!";
        CHECK(dptr_dev_id_[id] != -1) << id << " is Alloced for CPU!";
        unalloced_dptrs_.erase(ptr);
        size_t ptr_size = dptr_size_[ptr];
        void* new_ptr = Alloc_(ptr_size);
        dptr_mapping_[id] = new_ptr;
        sa_log << "GetDptr " << id << " Start setting dptr" << std::endl; 
        odswap_->SetAddr(id, new_ptr, ptr_size, 0, false);
      } else {
        bool tmp;
        void* new_ptr = odswap_->GetAddr(id, 1, tmp);
        dptr_mapping_[id] = new_ptr;  
      }
    } else { // Iteration 3, normal getaddr.
      bool tmp;
      void* new_ptr = odswap_->GetAddr(id, 0, tmp);
      dptr_mapping_[id] = new_ptr;
    }
    sa_log << "GetDtpr " << id << " return: " << dptr_mapping_[id] << std::endl;
    return dptr_mapping_[id];
  }

  void SetDptr (handle_t id, void* ptr, uint32_t dev_id) override {
    sa_log << "SetDptr " << id << " " << ptr << " " << dev_id  << std::endl;
    CHECK(ptr != fake_memory_) << "Fake memory is reasigned, which is dangerous!";
    size_t ptr_size = 0;
    if(ptr != nullptr && dev_id != -1) {
      CHECK(dptr_size_.find(ptr) != dptr_size_.end()) 
      << "Can't find the size for id " << id << ".";
      ptr_size = dptr_size_[ptr];
    }
    dptr_dev_id_[id] = dev_id;
    dptr_mapping_[id] = ptr;
    // SetDptr is only called in preparation stage.
    odswap_->SetAddr(id, ptr, ptr_size, dev_id, true);
  }

 private:
  void* Alloc_(size_t ptr_size) {
    sa_log << "Alloc_ size = " << ptr_size << std::endl;
    odswap_->SwapOutLocked(ptr_size, device_id_, false);
    void* ret = nullptr;
    cudaError_t e = memory_manager_->Malloc(ret, ptr_size, device_id_);
    if (e != cudaSuccess && e != cudaErrorCudartUnloading) {
      LOG(FATAL) << "cudaMalloc failed: " << cudaGetErrorString(e);
    }
    return ret;
  }

  // SharedRef for other modules.
  std::shared_ptr<ODSwap> odswap_;
  std::shared_ptr<MemoryManager> memory_manager_;
  std::shared_ptr<MemoryHistory> memory_history_;
  std::shared_ptr<Prefetch> prefetch_;
  std::unordered_set<handle_t> temp_handles_;
  std::unordered_set<void*> unalloced_dptrs_;
  // dptr_size_ is no longer usable after loop 1
  std::unordered_map<void*, size_t> dptr_size_;
  std::unordered_map<handle_t, void*> dptr_mapping_;
  // Use for checking cpu addr.
  std::unordered_map<handle_t, size_t> dptr_dev_id_;
  // Infos for node history
  std::vector<std::pair<node_t, std::string>> node_history_;
  std::map<std::pair<node_t, std::string>, std::unordered_set<handle_t>> node_handles_;
  std::pair<node_t, std::string> cur_node_;
  size_t cur_nid_idx_;
  void* temp_memory_;
  void* fake_memory_;
  size_t temp_size_;
  const float kGPUTempRatio = 3;
  int device_id_; // Only support dev_id = 0 currently.
};

}  // namespace storage
}  // namespace mxnet
#endif
