#include <dmlc/logging.h>
#include "../common/cuda_utils.h"
#include "./gpu_odswap.h"

#define FEGIN_DEBUG

namespace mxnet{

std::shared_ptr<ODSwap> ODSwap::_GetSharedRef() {
  static std::shared_ptr<ODSwap> inst(new ODSwap());
  return inst;
}

ODSwap* ODSwap::Get() {
  static ODSwap *s = _GetSharedRef().get();
  return s;
}

ODSwap::ODSwap() {
  std::cout << "Initialize ODSwap" << std::endl;
  swap_lock_ = PTHREAD_RWLOCK_INITIALIZER;
  swap_async_ = dmlc::GetEnv("MXNET_SWAP_ASYNC", true);
  sem_init(&swap_sem_, 0, 1);
  std::cout << "SWAP_ASYNC=" << swap_async_ << std::endl;
  infinite_memory_ = dmlc::GetEnv("MXNET_INFINITE_MEMORY", false);
  infinite_cpu_memory_ = dmlc::GetEnv("MXNET_INFINITE_CPU_MEMORY", false);
  if (infinite_cpu_memory_) {
    const size_t fake_cpu_size = 20L*1024*1024*1024;
    cudaHostAlloc((void**)&(fake_cpu_address_), fake_cpu_size, 0);
    CHECK(fake_cpu_address_ != nullptr)
      << "Fake cpu memory allocation failed" << std::endl;
    std::cout << "Initialize fake cpu memory of size: "
              << fake_cpu_size << "B" << std::endl;
  }
  for (int i = 0; i < NUMBER_OF_GPU; ++i) {
    // TODO(fegin): This and other cuda related code should be protected
    //              by MXNET_USE_CUDA.
    cudaStreamCreate(&streams_out_[i]);
    cudaStreamCreate(&streams_in_[i]);
  }
  memory_history_ = MemoryHistory::_GetSharedRef();
  memory_manager_ = GetMemoryManagerRef();
  std::cout << "Initialize ODSwap done" << std::endl;
}

ODSwap::~ODSwap() {
  std::cout << "Destroy ODSwap" << std::endl;
  cudaFreeHost(fake_cpu_address_);
}

void ODSwap::SwapOutLocked(unsigned required_memory, int device_id, bool async) {
  pthread_rwlock_wrlock(&swap_lock_);
  SwapOut(required_memory, device_id, async);
  pthread_rwlock_unlock(&swap_lock_);
}

// Caller holds swap_lock_
bool ODSwap::SwapOut(unsigned required_memory, int device_id, bool async) {
  sa_log << "Swapout: required memory = " << required_memory << std::endl;
  while (!memory_manager_->TryAllocate(device_id, required_memory)) {
    SwapParams param = {0, required_memory, &divided_handles_[device_id]};
#ifdef FEGIN_DEBUG
    sa_log << "Swapout calling Decide victim. Swappable size = "
           << swappable_handles_[device_id].size() << std::endl;
#endif
    // Error messages
    if(swappable_handles_[device_id].size() <= 0) {
      return false;
      /*
      std::cout << "Handle exhausted!" << std::endl;

      std::cout << "Handles that are swapped in:" << std::endl;
      size_t swap_total = 0;
      for (auto& tmp: swap_info_) {
        auto& info = tmp.second;
        if(info->swapped_in) {
          swap_total += info->size;
          std::cout << tmp.first << " size: " << info->size 
                 << " Address: " << info->dptr
                 << " Swap counts: " << info->swap_count << std::endl;
        }
      }
      std::cout << "Total Size of swapped in handles: " << swap_total << std::endl;

      std::cout << "Handles that are swapped out:" << std::endl;
      swap_total = 0;
      for (auto& tmp: swap_info_) {
        auto& info = tmp.second;
        if(!info->swapped_in) {
          swap_total += info->size;
          std::cout << tmp.first << " size: " << info->size 
                 << " Address: " << info->cpu_address
                 << " Swap counts: " << info->swap_count << std::endl;
        }
      }
      std::cout << "Total Size of swapped out handles: " << swap_total << std::endl;
      std::cout << "Sum of kept and swapped out handles: "
             << swap_total + kept_total << std::endl;

      std::cout << "Memory Info:" << std::endl;
      size_t total, avail;
      memory_manager_->MemGetInfo(0, &total, &avail);
      std::cout << "Total: " << total << " Avail: " << avail << std::endl;
      */
    }
    /*
    CHECK(swappable_handles_[device_id].size() > 0)
      << "Set of swappable handles is exhausted" << std::endl;
    */
    handle_t victim =
      memory_history_->DecideVictim(swappable_handles_[device_id], device_id,
                                    &param);
    CHECK(swap_info_.find(victim) != swap_info_.end())
      << "Victim(" << victim << ") does not exist (deleted?) " << std::endl;
    SwapInfo *target = swap_info_[victim];
#ifdef FEGIN_DEBUG
    sa_log << "SwapOut " << victim << " " << target->size << " "
           << target->swap_count << std::endl;
#endif
    target->swap_count++;
    memory_history_->DevHistory(device_id).num_swap_out++;
    memory_history_->DevHistory(device_id).swap_out_total += target->size;
    if (!infinite_memory_) {
      if (target->cpu_address == nullptr) {
        if (infinite_cpu_memory_) {
          target->cpu_address = fake_cpu_address_; 
        }
        else {
          cudaHostAlloc((void**)&(target->cpu_address), target->size, 0);
        }
      }
      CHECK(target->cpu_address != nullptr);
    }
    CHECK(target->swapped_in) << "Target " << target->handle_id
        << " is not swapped_in!" << std::endl;
    CHECK(!target->is_swapping.test_and_set(std::memory_order_acquire));
    CHECK(target->dptr != nullptr);
    target->swapped_in = false;
    swappable_handles_[device_id].erase(victim);
    divided_handles_[device_id][target->size].erase(victim);
#ifdef FEGIN_DEBUG
    sa_log << "SwapOut: Swapping out, remove(1) swappable handle_id = "
           << victim << std::endl;
#endif
    pthread_rwlock_unlock(&swap_lock_);
    if (!infinite_memory_) {
#if 1
      if (async) {
        memory_manager_->MemcpyAsync(device_id, target->cpu_address,
            target->dptr, target->size, cudaMemcpyDeviceToHost,
            streams_out_[device_id]);
        memory_manager_->StreamSynchronize(device_id, streams_out_[device_id]);
      } else {
        memory_manager_->Memcpy(device_id, target->cpu_address, target->dptr,
            target->size, cudaMemcpyDeviceToHost);
      }
#endif
    }
    memory_manager_->Free(target->dptr, device_id);
    pthread_rwlock_wrlock(&swap_lock_);
    target->is_swapping.clear(std::memory_order_release);
#ifdef FEGIN_DEBUG
    sa_log << "SwapOut: Finish swapping out handle: " << victim << std::endl; 
#endif
  }
  return true;
}

// Caller holds swap_lock_
bool ODSwap::SwapIn(SwapInfo *info, bool async) {
  while (info->is_swapping.test_and_set(std::memory_order_acquire)) {
    sa_log << "id " << info->handle_id << " is swapping" << std::endl;
    // TODO(fegin): usleep may not be efficient and may cause unstable
    //              execution. This is not important for now but can be
    //              something to imporve in the future. However, this
    //              must be designed carefully. One mutex per handle is not
    //              acceptable unlike current atmoic variable design.
    pthread_rwlock_unlock(&swap_lock_);
    usleep(10);
    pthread_rwlock_wrlock(&swap_lock_);
  }
  if (!info->swapped_in) {
    CHECK(!info->swapped_in);
    CHECK(info->cpu_address != nullptr || infinite_memory_);
#ifdef FEGIN_DEBUG
    sa_log << "SwapIn "<< info->handle_id << " " << info->size << " "
           << info->swap_count << std::endl;
#endif
    if (!SwapOut(info->size, info->device_id, async)) {
      info->is_swapping.clear(std::memory_order_release);
      return false;
    }
    CHECK(memory_manager_->Malloc(info->dptr, info->size, info->device_id) ==
          cudaSuccess);
    pthread_rwlock_unlock(&swap_lock_);
    memory_history_->DevHistory(info->device_id).num_swap_in++;
    memory_history_->DevHistory(info->device_id).swap_in_total += info->size;
    if (!infinite_memory_) {
#if 1
      if (async) {
        memory_manager_->MemcpyAsync(info->device_id, info->dptr,
            info->cpu_address, info->size, cudaMemcpyHostToDevice,
            streams_in_[info->device_id]);
        memory_manager_->StreamSynchronize(info->device_id,
              streams_in_[info->device_id]);
      } else {
        memory_manager_->Memcpy(info->device_id, info->dptr, info->cpu_address,
                                info->size, cudaMemcpyHostToDevice);
      }
#endif
      // delete info->cpu_address;
      // info->cpu_address = nullptr;
    }
    sa_log << "Swapin: memcpy for handle " << info->handle_id 
           << " is over" << std::endl;
    pthread_rwlock_wrlock(&swap_lock_);
    info->swapped_in = true;
    // With new design, swappable handle is updated only at start/end/access
    //swappable_handles_[info->device_id].insert(info->handle_id);
    //divided_handles_[info->device_id][info->size].insert(info->handle_id);
#ifdef FEGIN_DEBUG
    sa_log << "Insert(1) swappable handle_id = "
           << info->handle_id << std::endl;
#endif
  }
  info->is_swapping.clear(std::memory_order_release);
  return true;
}

void ODSwap::SetAddr(handle_t handle_id, void* dptr, size_t size,
                     int device_id, bool is_pre) {
  if (device_id != -1 && is_pre) {
    memory_history_->PutRecord(handle_id, device_id, MemoryHistory::SET_ADDR,
                               size);
  }
#ifdef FEGIN_DEBUG
  sa_log << "SetAddr=" << handle_id << ", size=" << size <<  std::endl;
#endif
  if (dptr == nullptr) {
    return;
  }
  pthread_rwlock_wrlock(&swap_lock_);
  auto iter = swap_info_.find(handle_id);
  if (is_pre) {
    CHECK(iter == swap_info_.end())
      << "Same info is set twice in preparation stage" << std::endl;
    SwapInfo* info = new SwapInfo{handle_id, true, device_id,
      dptr, nullptr, size, 0, ATOMIC_FLAG_INIT, false};
    swap_info_[handle_id] = info;
  } else {
    CHECK(iter != swap_info_.end());
    iter->second->dptr = dptr;
  }
  pthread_rwlock_unlock(&swap_lock_);
  #ifdef FEGIN_DEBUG
      sa_log << "SetAddr " << handle_id << " Returning"<< std::endl;
  #endif
}

void ODSwap::FreeAddr(handle_t handle_id) {
  pthread_rwlock_wrlock(&swap_lock_);
  //std::cout << "FreeAddr " << handle_id << std::endl;
  auto info = swap_info_.at(handle_id);
  if (info->device_id != -1) {
    memory_history_->PutRecord(handle_id, info->device_id,
                               MemoryHistory::DEL_ADDR, info->size);
    if (swappable_handles_[info->device_id].find(handle_id)
        != swappable_handles_[info->device_id].end()) {
      swappable_handles_[info->device_id].erase(handle_id);
    }
    if (divided_handles_[info->device_id][info->size].find(handle_id)
        != divided_handles_[info->device_id][info->size].end()) {
      divided_handles_[info->device_id][info->size].erase(handle_id);
    }
  }
  size_t free, total;
  memory_manager_->MemGetInfo(info->device_id, &total, &free);
  if (info->swapped_in) {
    memory_manager_->Free(info->dptr, info->device_id);
  }
  if (info->cpu_address != nullptr) {
    //delete info->cpu_address;
    cudaFreeHost(info->cpu_address);
  }
  delete info;
  swap_info_.erase(handle_id);
  pthread_rwlock_unlock(&swap_lock_);
}

void ODSwap::DelAddr(handle_t handle_id) {
  pthread_rwlock_wrlock(&swap_lock_);
  //std::cout << "DelAddr " << handle_id << std::endl;
  auto info = swap_info_.at(handle_id);
  if (info->device_id != -1) {
    memory_history_->PutRecord(handle_id, info->device_id,
                               MemoryHistory::DEL_ADDR, info->size);
    if (swappable_handles_[info->device_id].find(handle_id)
        != swappable_handles_[info->device_id].end()) {
      swappable_handles_[info->device_id].erase(handle_id);
#ifdef FEGIN_DEBUG
      sa_log << "Remove(2) swappable handle_id = " << handle_id
             << std::endl;
#endif
    }
    if (divided_handles_[info->device_id][info->size].find(handle_id)
        != divided_handles_[info->device_id][info->size].end()) {
      divided_handles_[info->device_id][info->size].erase(handle_id);
    }
  }
  if (info->cpu_address != nullptr) {
    //delete info->cpu_address;
    if (!infinite_cpu_memory_) {
      cudaFreeHost(info->cpu_address);
    }
  }
  delete info;
  swap_info_.erase(handle_id);
  pthread_rwlock_unlock(&swap_lock_);
}

// type: 0 normal, 1 allocate, 2 prefetch
void* ODSwap::GetAddr(handle_t handle_id, int type, bool& success) {
#ifdef FEGIN_DEBUG
  sa_log << "GetAddr[" << type << "]: "<< handle_id << std::endl;
  size_t total, avail;
  memory_manager_->MemGetInfo(0, &total, &avail);
  sa_log << "Memory Info: Total: " << total << " Avail: " << avail << std::endl;
#endif
  pthread_rwlock_wrlock(&swap_lock_);
  auto info = swap_info_.at(handle_id);
  if (info->device_id != -1 && type == 0) {
    memory_history_->DevHistory(info->device_id).num_get_addr++;
    memory_history_->PutRecord(handle_id, info->device_id,
                               MemoryHistory::GET_ADDR, info->size);
  }
#ifdef FEGIN_DEBUG
  sa_log << "GetAddr info size = " << info->size << std::endl;
#endif
  if (!info->swapped_in && type > 0) {
    /*
    if (!prefetch) {
      ++(memory_history_->DevHistory(info->device_id).cache_miss);
    }
    */
    success = SwapIn(info, swap_async_);
    if (!success) {
      pthread_rwlock_unlock(&swap_lock_);
      return nullptr;
    }
  } else if (!info->swapped_in) {
    sa_log << "Handle " << handle_id << " is not prefetched." << std::endl;
    info->is_waiting = true;
    pthread_rwlock_unlock(&swap_lock_);
    while (!info->swapped_in) {
      sem_wait(&swap_sem_);
    };
    pthread_rwlock_wrlock(&swap_lock_);
    info->is_waiting = false;
  } else {
    success = true;
  }
  CHECK(info->swapped_in) << "Info " 
     << info->handle_id << " is not swapped in after SwapIn" << std::endl;
  // Lock handle that is prefetched
  if (type == 2) {
    auto pair = locked_handles_.emplace(handle_id, 1);
    if (!pair.second) {
      pair.first->second++;
    }
    sa_log << "GetAddr add counter unswappable handle_id = " << handle_id
           << " counter: " << pair.first->second << std::endl;
  }
  // Remove from swappable handles
  swappable_handles_[info->device_id].erase(handle_id);
  divided_handles_[info->device_id][info->size].erase(handle_id);
#ifdef FEGIN_DEBUG
  sa_log << "Remove(3) swappable handle_id = " << handle_id << std::endl;
#endif
  pthread_rwlock_unlock(&swap_lock_);
  if (info->is_waiting) {
    sem_post(&swap_sem_);
  }
  return info->dptr;
}

void ODSwap::StartComputing(const std::unordered_set<handle_t>& handles) {
  pthread_rwlock_wrlock(&swap_lock_);
  for (auto handle: handles) {
    auto pair = locked_handles_.emplace(handle, 1);
    if (!pair.second) {
      pair.first->second++;
    }
    sa_log << "StartComputing add counter unswappable handle_id = " << handle
           << " counter: " << pair.first->second << std::endl;
  }
  pthread_rwlock_unlock(&swap_lock_);
}

void ODSwap::StopComputing(const std::unordered_set<handle_t>& handles) {
  pthread_rwlock_wrlock(&swap_lock_);
  for (auto& handle: handles) {
    CHECK(locked_handles_.find(handle) != locked_handles_.end());
    locked_handles_[handle]--;
    sa_log << "Reduce 1 from counter for handle_id = " <<  handle 
           << " counter: " << locked_handles_[handle] << std::endl;
    if (locked_handles_[handle] <= 0) {
      sa_log << "Insert swappable handle_id = " <<  handle << std::endl;
      auto it = swap_info_.find(handle);
      swappable_handles_[it->second->device_id].insert(handle);
      divided_handles_[it->second->device_id][it->second->size].insert(handle);
    }
  }
  pthread_rwlock_unlock(&swap_lock_);
}

void ODSwap::PrintHandles() {
  std::cout << "Print Handles" << std::endl;
  //std::map<size_t, std::unordered_set<handle_t> > _divided_handles_;
  for (auto it : swap_info_) {
    //_divided_handles_[it.second->size].insert(it.first);
    std::cout << it.first << ": " << it.second->size << " "
              << it.second->swap_count << " " << it.second->device_id
              << std::endl;
  }
}

} // namespace mxnet
