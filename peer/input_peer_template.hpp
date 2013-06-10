#ifndef INPUT_PEER_TEMPLATE_HPP_
#define INPUT_PEER_TEMPLATE_HPP_

#include <common.hpp>
#include <bgp/bgp.hpp>
#include <bgp/vertex.hpp>

#include <bitset>

#include <gsl/gsl_interp.h>
#include <gsl/gsl_poly.h>

#include <boost/dynamic_bitset.hpp>


typedef Secret<plaintext_t, COMP_PEER_NUM> secret_t;


template<size_t Num>
void InputPeer::result() {}



template<class CompPeerSeq>
void InputPeer::distribute_secret(
    symbol_t key, plaintext_t value,
    CompPeerSeq& comp_peers) {

  secret_t secret(value);
  auto shares = secret.share();
  distribute_shares(key, shares, value, 0, comp_peers);

}



template<class PlaintextMap, class CompPeerSeq>
void InputPeer::distribute_lsb(
    const PlaintextMap& secret_map,
    CompPeerSeq& comp_peers) {

}



template<class PlaintextMap, class CompPeerSeq>
void InputPeer::distribute_secrets(
    const PlaintextMap& secret_map,
    CompPeerSeq& comp_peers) {

  for(auto pair : secret_map) {
    distribute_secret(pair.first, pair.second, comp_peers);
  }

}



template<class CompPeerSeq>
void InputPeer::bitwise_share(string key, int64_t value, CompPeerSeq& comp_peers) {

  size_t size = 64;
  boost::dynamic_bitset<> bs(size, value);

  string bits;
  for(auto i = 0; i < size; i++) {
    bits += lexical_cast<string>( bs[i] );
  }

  LOG4CXX_DEBUG(logger_, "Sharing bitset: " << bits);

  for(auto i = 0; i < size; i++) {
    LOG4CXX_DEBUG(logger_, "Sharing bit " << i << ": ");

    const string symbol = key + "b" + lexical_cast<string>(i);
    const auto bit = bs[i];

    secret_t secret(bit);
    auto shares = secret.share();
    distribute_shares(symbol, shares, bit, 0, comp_peers);
  }
}



template<class CompPeerSeq>
void InputPeer::lsb(
    string key,
    int64_t value,
    CompPeerSeq& comp_peers) {

  key = ".2" + key;
  value = 2 * value;

  int result = mod(value, PRIME);
  result = result % 2;

  LOG4CXX_DEBUG(logger_, "LSB (" << key << "): " << result);

  distribute_secret(key, result, comp_peers);
}




template<class CompPeerSeq>
vector<update_vertex_t> InputPeer::start_listeners(CompPeerSeq& comp_peers, graph_t& input_graph) {

  vector<update_vertex_t> nodes;

  auto iter = vertices(input_graph);
  auto last = iter.second;
  auto current = iter.first;

  for (; current != last; ++current) {
    const auto& current_vertex = *current;

    update_vertex_t update;
    update.vertex_ = current_vertex;
    update.next_hop_ = Vertex::UNDEFINED;
    nodes.push_back(update);

    for(size_t i = 0; i < COMP_PEER_NUM; i++) {
      size_t port = START_PORT + COMP_PEER_NUM*current_vertex + i;
      auto cp = comp_peers[i];
      try {

        auto sp = shared_ptr<RPCServer>(new RPCServer(cp->io_service_, port, cp.get() ) );
        for(auto ccp: comp_peers) {
          Vertex& vertex = ccp->bgp_->graph_[current_vertex];
          vertex.servers_[i] = sp;
        }

      } catch (...) {
        std::cout << "Port number in question: " << port << std::endl;
        throw;
      }
    }
  }

  return nodes;
}





template<class CompPeerSeq>
void InputPeer::start_clients(
    CompPeerSeq& comp_peers,
    graph_t& input_graph,
    sync_response::hostname_mappings_t* hm) {

  auto iter = vertices(input_graph);
  auto last = iter.second;
  auto current = iter.first;

  for (; current != last; ++current) {
    const auto& current_vertex = *current;

    LOG4CXX_TRACE(logger_, "Current vertex: " << current_vertex);

    for(size_t i = 0; i < COMP_PEER_NUM; i++) {
      size_t port = START_PORT + COMP_PEER_NUM*current_vertex + i;

      for(size_t ID = 1; ID <= COMP_PEER_NUM; ID++) {

        if (COMP_PEER_IDS.find(ID) == COMP_PEER_IDS.end()) continue;

        auto cp  = comp_peers[ID - 1];
        auto sp = shared_ptr<RPCClient>(
        new RPCClient(cp->io_service_, (*hm)[i + 1][current_vertex], port));

        for(auto ccp: comp_peers) {
          Vertex& vertex = ccp->bgp_->graph_[current_vertex];
          vertex.clients_[ID][i] = sp;
        }

      }
    }
  }

}



template<class CompPeerSeq>
void InputPeer::disseminate_bgp(CompPeerSeq& comp_peers, graph_t& input_graph) {
}



#endif /* INPUT_PEER_TEMPLATE_HPP_ */
