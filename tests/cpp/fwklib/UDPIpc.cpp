/*
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "UDPIpc.hpp"

#include <sstream>
#include <string>
#include <vector>

#include "FwkStrCvt.hpp"
#include "GsRandom.hpp"
#include "config.h"

namespace apache {
namespace geode {
namespace client {
namespace testframework {

bool UDPMessage::ping(ACE_SOCK_Dgram& io, ACE_INET_Addr& who) {
  clear();
  setCmd(ACK_REQUEST);
  setSender(who);
  return send(io);
}

std::string UDPMessage::dump(int32_t max) {
  char buf[1024];
  std::string dmp("Dump of message parts: ");
  sprintf(buf, "\nTag: %u\nCmd: %u  %s\nId: %u\nLength: %u\nMessage:\n",
          m_hdr.tag, m_hdr.cmd, cmdString(m_hdr.cmd), m_hdr.id,
          ntohl(m_hdr.length));
  dmp += buf;
  if (!m_msg.empty()) {
    if (max > 0) {
      dmp += m_msg.substr(0, max);
    } else {
      dmp += m_msg;
    }
  } else {
    dmp += "No Message.";
  }
  dmp += "\nEnd of Dump";
  return dmp;
}

bool UDPMessage::receiveFrom(ACE_SOCK_Dgram& io,
                             const ACE_Time_Value* timeout) {
  bool ok = true;
  ACE_Time_Value wait(2);
  if (!timeout) {
    timeout = &wait;
  }
  iovec buffs;
  int32_t red = static_cast<int32_t>(io.recv(&buffs, m_sender, 0, timeout));
  if (red < 0) {
    if (errno != ETIME) {
      FWKEXCEPTION("UDPMessage::receiveFrom: Failed, errno: " << errno);
    }
    return false;
  }
  uint32_t len = buffs.iov_len;
  if (len < UDP_HEADER_SIZE) {  // We must at least have a header
    FWKEXCEPTION("UDPMessage::receiveFrom: Failed, header length: " << len);
  }
  clear();
  memcpy(&m_hdr, buffs.iov_base, UDP_HEADER_SIZE);
  if (m_hdr.tag != UDP_MSG_TAG) {
    FWKEXCEPTION("UDPMessage::receiveFrom: Failed, invalid tag: " << m_hdr.tag);
  }
  char* ptr = reinterpret_cast<char*>(buffs.iov_base);
  ptr += UDP_HEADER_SIZE;
  len -= UDP_HEADER_SIZE;
  uint32_t sent = ntohl(m_hdr.length);
  if (sent != len) {
    FWKEXCEPTION("UDPMessage::receiveFrom: Failed, expected "
                 << sent << " bytes, received " << len);
  }
  if (len > 0) {
    m_msg = std::string(ptr, len);
  }
  delete[] reinterpret_cast<char*>(buffs.iov_base);
  // FWKINFO( "UDPMessage::receiveFrom: " << dump( 50 ) );;
  if (needToAck()) {
    UDPMessage ack(ACK);
    ok = (ok && ack.sendTo(io, m_sender));
  }
  return ok;
}

bool UDPMessage::sendTo(ACE_SOCK_Dgram& io, ACE_INET_Addr& who) {
  setSender(who);
  return send(io);
}

bool UDPMessage::send(ACE_SOCK_Dgram& io) {
  bool ok = true;
  int32_t tot = 0;
  int32_t vcnt = 1;
  iovec buffs[2];
  m_hdr.length = 0;
  buffs[0].iov_base = reinterpret_cast<char*>(&m_hdr);
  buffs[0].iov_len = UDP_HEADER_SIZE;
  tot += UDP_HEADER_SIZE;
  if (!m_msg.empty()) {
    auto len = static_cast<uint32_t>(m_msg.size());
    m_hdr.length = htonl(len);
    buffs[1].iov_base = const_cast<char*>(m_msg.c_str());
    buffs[1].iov_len = len;
    vcnt = 2;
    tot += len;
  }
  // FWKINFO( "UDPMessage::send: " << dump( 50 ) );;
  int32_t sent = static_cast<int32_t>(io.send(buffs, vcnt, m_sender));
  if (sent < 0) {
    FWKEXCEPTION("UDPMessage::send: Failed, errno: " << errno);
  }
  if (sent != tot) {
    ok = false;
    FWKSEVERE("UDPMessage::send: Failed to completely send, " << sent << ", "
                                                              << tot);
  }
  if (needToAck()) {
    UDPMessage ack;
    ok = (ok && ack.receiveFrom(io));
  }
  return ok;
}

UDPMessageClient::UDPMessageClient(std::string server)
    : m_server(server.c_str()) {
  int32_t result = -1;
  int32_t tries = 100;
  ACE_INET_Addr* client = new ACE_INET_Addr();
  while ((result < 0) && (tries > 0)) {
    uint32_t port = GsRandom::random(1111u, 31111u) + tries;
    client->set(port, "localhost");
    result = m_io.open(*client);
  }
  delete client;
  if (result < 0) {
    FWKEXCEPTION("Client failed to open io, " << errno);
  }
  const ACE_Time_Value timeout(20);
  UDPMessage msg;
  tries = 3;
  bool pingMsg = false;
  while (!pingMsg && tries-- > 0) {
    try {
      pingMsg = msg.ping(m_io, m_server);
    } catch (...) {
      continue;
    }
  }
  if (pingMsg) {
    // if ( msg.ping( m_io, m_server ) ) {
    msg.setSender(m_server);
    bool connectionOK = false;
    tries = 10;
    while (!connectionOK && (--tries > 0)) {
      try {
        msg.clear();
        msg.setCmd(ADDR_REQUEST);
        if (msg.sendTo(m_io, m_server)) {
          if (msg.receiveFrom(m_io, &timeout)) {
            std::string newConn = msg.what();
            ACE_INET_Addr conn(newConn.c_str());
            if (msg.ping(m_io, conn)) {  // We have a working addr
              m_server = conn;
              connectionOK = true;
              tries = 0;
            }
          }
        }
      } catch (...) {
        continue;
      }
    }
    if (!connectionOK) {
      FWKEXCEPTION(
          "UDPMessageClient failed to establish connection to server.");
    }
  } else {
    FWKEXCEPTION("Failed to contact " << server);
  }
}

int32_t Receiver::doTask() {
  auto msg = std::unique_ptr<UDPMessage>(new UDPMessage());
  UDPMessage cmsg;
  try {
    while (*m_run) {
      if (isListener()) {
        cmsg.clear();
        ACE_Time_Value wait(30);  // Timeout is relative time.
        if (cmsg.receiveFrom(*m_io, &wait)) {
          if (cmsg.getCmd() == ADDR_REQUEST) {
            auto&& addr = m_addrs.front();
            m_addrs.pop_front();
            m_addrs.push_back(addr);
            cmsg.clear();
            cmsg.setMessage(addr);
            cmsg.setCmd(ADDR_RESPONSE);
            cmsg.send(*m_io);
          }
        }
      } else {
        msg->clear();
        ACE_Time_Value timeout(2);
        if (msg->receiveFrom(
                *m_io, &timeout)) {  // Timeout is relative time, send ack.
          if (msg->getCmd() == ADDR_REQUEST) {
            auto&& addr = m_addrs.front();
            m_addrs.pop_front();
            m_addrs.push_back(addr);
            cmsg.clear();
            cmsg.setMessage(addr);
            cmsg.setCmd(ADDR_RESPONSE);
            cmsg.send(*m_io);
          }
          if (msg->length() > 0) {
            m_queues->putInbound(msg.release());
            msg.reset(new UDPMessage());
          }
        }
      }
    }
  } catch (FwkException& ex) {
    FWKSEVERE("Receiver::doTask() caught exception: " << ex.what());
  } catch (...) {
    FWKSEVERE("Receiver::doTask() caught unknown exception");
  }
  return 0;
}

void Receiver::initialize() {
  int32_t tries = 100;
  uint16_t port = m_basePort;
  int32_t lockResult = m_mutex.tryacquire();
  int32_t result = -1;
  if (lockResult != -1) {  // The listener thread
    ACE_INET_Addr addr(port, "localhost");
    m_listener = ACE_Thread::self();
    result = m_io->open(addr);
  } else {
    while ((result < 0) && (--tries > 0)) {
      port += ++m_offset;
      ACE_INET_Addr addr(port, "localhost");
      result = m_io->open(addr);
      if (result == 0) {
        char hbuff[256];
        char* hst = &hbuff[0];
        char* fqdn = ACE_OS::getenv("GF_FQDN");
        if (fqdn) {
          hst = fqdn;
        } else {
          addr.get_host_name(hbuff, 255);
        }
        char buff[1024];
        sprintf(buff, "%s:%u", hst, port);
        m_addrs.push_back(buff);
      }
    }
  }
  if (result < 0) {
    FWKEXCEPTION("Server failed to open io, " << errno << ", on port " << port);
  }
}

int32_t STReceiver::doTask() {
  auto msg = std::unique_ptr<UDPMessage>(new UDPMessage());
  try {
    while (*m_run) {
      msg->clear();
      ACE_Time_Value timeout(2);  // Timeout is relative time
      if (msg->receiveFrom(m_io, &timeout)) {
        if (msg->getCmd() == ADDR_REQUEST) {
          msg->clear();
          msg->setMessage(m_addr);
          msg->setCmd(ADDR_RESPONSE);
          msg->send(m_io);
        } else {
          if (msg->length() > 0) {
            m_queues->putInbound(msg.release());
            msg.reset(new UDPMessage());
          }
        }
      }
    }
  } catch (FwkException& ex) {
    FWKSEVERE("STReceiver::doTask() caught exception: " << ex.what());
  } catch (...) {
    FWKSEVERE("STReceiver::doTask() caught unknown exception");
  }
  return 0;
}

void STReceiver::initialize() {
  int32_t result = -1;
  ACE_INET_Addr addr(m_basePort, "localhost");
  result = m_io.open(addr);
  if (result == 0) {
    char hbuff[256];
    char* hst = &hbuff[0];
    char* fqdn = ACE_OS::getenv("GF_FQDN");
    if (fqdn) {
      hst = fqdn;
    } else {
      addr.get_host_name(hbuff, 255);
    }
    char buff[1024];
    sprintf(buff, "%s:%u", hst, m_basePort);
    m_addr = buff;
  }
  if (result < 0) {
    FWKEXCEPTION("STReceiver::initialize failed to open io, "
                 << errno << ", on port " << m_basePort);
  }
}

int32_t Responder::doTask() {
  try {
    while (*m_run) {
      UDPMessage* msg = m_queues->getOutbound();
      if (msg) {
        msg->send(*m_io);
        delete msg;
      }
    }
  } catch (FwkException& ex) {
    FWKSEVERE("Responder::doTask() caught exception: " << ex.what());
  } catch (...) {
    FWKSEVERE("Responder::doTask() caught unknown exception");
  }
  return 0;
}

void Responder::initialize() {
  int32_t result = -1;
  int32_t tries = 100;
  while ((result < 0) && (--tries > 0)) {
    uint16_t port = ++m_offset + 111 + m_basePort;
    result = m_io->open(ACE_INET_Addr(port, "localhost"));
    if (result < 0) {
      FWKWARN("Server failed to open io, " << errno << ", on port " << port);
    }
  }
  if (result < 0) {
    FWKEXCEPTION("Server failed to open io, " << errno);
  }
}

}  // namespace testframework
}  // namespace client
}  // namespace geode
}  // namespace apache
