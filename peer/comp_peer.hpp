#ifndef COMP_PEER_HPP_
#define COMP_PEER_HPP_

#include <common.hpp>

#include <map>
#include <deque>
#include <atomic>

#include <tbb/concurrent_vector.h>
#include <tbb/concurrent_queue.h>

#include <boost/array.hpp>
#include <boost/shared_ptr.hpp>

#include <gsl/gsl_blas.h>

#include <bgp/bgp.hpp>
#include <peer/peer.hpp>
#include <peer/input_peer.hpp>

class InputPeer;

template<const size_t Num>
class CompPeer : public Peer {
public:

  CompPeer(
      size_t id,
      shared_ptr<InputPeer> input_peer,
      std::unordered_map<int, shared_ptr<boost::barrier> > b,
      io_service& io);
  ~CompPeer();


  void evaluate(vector<string> circut, vertex_t l);
  void evaluate(string a, string b);

  const string get_recombination(vector<string>& circut);
  void execute(vector<string> circut, vertex_t l);

  void add(string first, string second, string recombination_key, vertex_t l);
  void sub(string first, string second, string recombination_key, vertex_t l);
  void multiply(string first, string second, string recombination_key, vertex_t l);
  void multiply_eq(string first, string recombination_key, vertex_t l);
  void multiply_const(string first, int64_t second, string recombination_key, vertex_t l);
  void recombine(string recombination_key, vertex_t key);

  void generate_random_num(string key, vertex_t l);
  void generate_random_bit(string key, vertex_t l);
  void generate_random_bitwise_num(string key, vertex_t l);

  void distribute(string final_key, int64_t value, vertex_t key);
  void interpolate(string final_key, vertex_t key);

  void publish(std::string key,  int64_t value, vertex_t update);

  void continue_or_not(vector<string> circut,
      const string key,
      const int64_t result,
      string recombination_key,
      vertex_t l);

  void publish_all(symbol_t key,  int64_t value);

  shared_ptr<BGPProcess> bgp_;

  boost::random::random_device rng_;

  size_t id_;

  typedef array<shared_ptr<CompPeer<Num> >, Num> localPeersImpl;
  typedef array<shared_ptr<RPCClient>, Num> netPeersImpl;

  netPeersImpl net_peers_;
  shared_ptr<InputPeer> input_peer_;

  gsl_vector* recombination_vercor_;
  array<double, Num> recombination_array_;
  std::unordered_map<int, shared_ptr<boost::barrier> > barrier_map_;
};

typedef CompPeer<COMP_PEER_NUM> comp_peer_t;


#include <peer/comp_peer_template.hpp>

#endif /* COMPPEER_HPP_ */
