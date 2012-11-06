/*
 * rpc.cpp
 *
 *  Created on: Nov 3, 2012
 *      Author: vjeko
 */

#include <net/rpc_client.hpp>


LoggerPtr RPCClient::logger_(Logger::getLogger("RPC Client"));


RPCClient::RPCClient(io_service& io_service, string hostname, int port) :
  socket_(io_service), resolver_(io_service) {

  const string service = lexical_cast<string>(port);

  LOG4CXX_TRACE(logger_, "Connecting to: " << service);

  tcp::resolver::query query(tcp::v4(), hostname, service);
  tcp::resolver::iterator iterator = resolver_.resolve(query);

  boost::asio::connect(socket_, iterator);
}



void RPCClient::publish(string key, int value) {

  char* data = new char[length_];

  memcpy(
      data,
      key.c_str(),
      length_ - sizeof(int32_t));

  memcpy(
      data + (length_ - sizeof(int32_t)),
      &value,
      sizeof(int32_t));


  LOG4CXX_TRACE(logger_, "Sending value: " << key << ": " << value);

  boost::asio::async_write(socket_, boost::asio::buffer(data, length_),
      boost::bind(&RPCClient::handle_write, this,
          boost::asio::placeholders::error,
          boost::asio::placeholders::bytes_transferred));
}



void RPCClient::handle_write(
      const boost::system::error_code& error,
      size_t bytes_transferred) {
}