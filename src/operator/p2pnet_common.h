/*!
 * Copyright (c) 2017 by Contributors
 * \file p2pnet_common.h
 * \brief
 * \author Chien-Chin Huang
*/
#ifndef MXNET_OPERATOR_NET_COMMON_H_
#define MXNET_OPERATOR_NET_COMMON_H_
#include <dmlc/logging.h>
#include <map>
#include <mutex>
#include <mxnet/engine.h>
#include <mxnet/operator.h>
#include <mxnet/ndarray.h>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <mutex>
#include <zmq.h>
#include "./operator_common.h"

namespace mxnet {
namespace op {

class P2PNet {
 public:
  static P2PNet& Get() { 
    static P2PNet instance;
    return instance;
  }
  P2PNet(P2PNet const&) = delete;
  void operator=(P2PNet const&) = delete;
  ~P2PNet();
 
  bool Init(const std::string& address_);

  void Start();

  enum RequestType {
    NewIndexRequest,
    AddRequest,
    SendRequest,
    RecvRequest,
  };
  struct Request {
    RequestType type;
    std::string address;
    unsigned tensor_id;
    void* buffer;
    size_t buffer_size;
    std::vector<NDArray*> ndptrs;
    engine::CallbackOnComplete on_complete;
  };
  void DoRequest(struct Request* request);

 private:
  P2PNet();
  void Main();
  void DoInternalRequest(size_t request_index); 
  void DoExternalRequest(); 
  void FreeRequest(struct Request* request);
  void DoSend(std::string& receiver_identity, struct Request* request); 

  std::mutex mtx; // mutex lock for request_queue_

  void* zmq_context_;
  // Every worker contains a server socket to allow other workers to connect to .
  void* server_; 
  // Every worker contains a internal server socket to allow send/recv requests 
  // from MXNet send/recv operators.
  void* internal_server_; 
  // Every worker contains a list of sockets representing the workers it
  // connects to.
  std::vector<void*> clients_;
  // The p2pnet main thread uses zmq inproc to communicate with send/receive 
  // operators which running in MXNet's threads.
  std::vector<void*> controls_;
  bool is_main_start_;
  bool is_bind_;
  zmq_pollitem_t* poll_items_;
  size_t poll_items_count_;
  std::thread* main_thread_;
  std::vector<struct Request*> request_queue_;
  std::map<unsigned, std::string> remote_request_queue_;
  std::map<unsigned, size_t> send_request_queue_;
  std::map<unsigned, size_t> recv_request_queue_;
  std::map<unsigned, void*> recv_request_sockets_;
  std::map<size_t, unsigned> recv_request_poll_indices;
  // TODO: We should remove this queue. If we can use serializaion and
  // and send only one message to Main() thread when a send/recv operator
  // needs to add a request, then this mapping is not needed.
  // std::map<std::string, size_t> request_index_mapping_;
  // Current Solution: Use mutex lock
};

}  // namespace op
}  // namespace mxnet
#endif // MXNET_OPERATOR_NET_COMMON_H_
