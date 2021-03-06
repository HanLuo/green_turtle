#include <sys/uio.h>
#include "buffered_socket.h"
#include "addr_info.h"
#include "socket_option.h"
#include "socket_config.h"
#include "event_loop.h"
#include <logger.h>

using namespace green_turtle;
using namespace green_turtle::net;

BufferedSocket::BufferedSocket(int fd, const AddrInfo& addr)
    : EventHandler(fd),
      addr_(addr),
      rcv_buffer_(new Buffer(SocketConfig::kBufferedSocketBufferSize)),
      snd_buffer_(new Buffer(SocketConfig::kBufferedSocketBufferSize)),
      write_lock_() {}

BufferedSocket::~BufferedSocket() {}
const AddrInfo& BufferedSocket::addr() const { return this->addr_; }

int BufferedSocket::OnRead() {
  char extrabuff[64 * 1024];
  iovec iov[2];
  iov[0].iov_base = rcv_buffer_->BeginWrite();
  iov[0].iov_len = rcv_buffer_->WritableLength();
  iov[1].iov_base = extrabuff;
  iov[1].iov_len = sizeof(extrabuff);

  int nread = SocketOption::Readv(fd(), iov, 2);
  if (nread < 0) return kErr;
  if (nread) {
    if (rcv_buffer_->WritableLength() <= (size_t)nread) {
      nread -= rcv_buffer_->WritableLength();
      rcv_buffer_->HasWritten(rcv_buffer_->WritableLength());
    } else {
      rcv_buffer_->HasWritten(nread);
      nread = 0;
    }
    if (nread) {
      rcv_buffer_->Append(extrabuff, nread);
    }
    Decoding(*rcv_buffer_);
    rcv_buffer_->Retrieve();
  }
  return kOK;
}

bool BufferedSocket::HasData() const {
  if (snd_messages_.size() || snd_buffer_->ReadableLength()) return true;
  return false;
}

int BufferedSocket::OnWrite() {
  if (!HasData()) return kOK;
  this->tmp_messages_.clear();
  {
    std::lock_guard<std::mutex> lock(this->write_lock_);
    this->tmp_messages_.swap(this->snd_messages_);
  }
  for (const auto& message : tmp_messages_) {
    const char* data = (char*)message->data();
    unsigned int len = message->length();

    snd_buffer_->Append(data, len);
  }

  int send_size = SocketOption::Write(fd(), snd_buffer_->BeginRead(),
                                      snd_buffer_->ReadableLength());
  if (send_size < 0)
    return kErr;
  else if (!send_size)
    return kOK;
  snd_buffer_->HasRead(send_size);

  if (snd_buffer_->ReadableLength() < SocketConfig::kBufferedSocketRetrieveBufferSize) {
    snd_buffer_->Retrieve();
  }
  return kOK;
}

int BufferedSocket::OnError() {
  ERROR_LOG(Logger::Default())("BufferedSocket(", this->addr().addr_str, ':', this->addr().addr_port, ") Error:", errno);
  this->event_loop()->RemoveEventHandler(this);
  return -1;
}

void BufferedSocket::SendMessage(std::shared_ptr<Message>&& data) {
  std::lock_guard<std::mutex> lock(this->write_lock_);
  this->snd_messages_.emplace_back(std::move(data));
}

void BufferedSocket::SendMessage(const std::shared_ptr<Message>& data) {
  std::lock_guard<std::mutex> lock(this->write_lock_);
  this->snd_messages_.push_back(data);
}
