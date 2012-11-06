
#ifndef COMP_PEER_TEMPLATE_HPP_
#define COMP_PEER_TEMPLATE_HPP_

#include <peer/comp_peer.hpp>



template<const size_t Num>
Comp_peer<Num>::Comp_peer(size_t id, shared_ptr<Input_peer> input_peer) :
    Peer(id + 10000),
    id_(id),
    input_peer_(input_peer) {

  shared_ptr<gsl_vector> x( gsl_vector_alloc(Num) );

  gsl_vector_set(x.get(), 0, 3);
  gsl_vector_set(x.get(), 1, -3);
  gsl_vector_set(x.get(), 2, 1);

  recombination_vercor_ = x;
}



template<const size_t Num>
void Comp_peer<Num>::execute(vector<string> circut) {

  auto first_operand = circut.back();
  circut.pop_back();

  auto second_operand = circut.back();
  circut.pop_back();

  auto operation = circut.back();
  circut.pop_back();

  recombination_key_ = first_operand + "_" + second_operand;
  const string key = recombination_key_ + boost::lexical_cast<string>(id_);
  const int result = values_[first_operand] * values_[second_operand];

  typedef array<shared_ptr<comp_peer_t>, Num> peer_array_t;
  typedef array<shared_ptr<RPCClient>, Num> peer_array__t;

  //new boost::thread(&Comp_peer::distribute_secret<peer_array_t>, this, key, result, all_peers_);
  new boost::thread(&Comp_peer::distribute_secret<peer_array__t>, this, key, result, all_peers__);
  //distribute_secret(key, result, all_peers__);
  recombine(circut);
}



template<const size_t Num>
void Comp_peer<Num>::recombine(vector<string> circut) {

  condition_.wait(lock_);

  shared_ptr<gsl_vector> ds( gsl_vector_alloc(3) );

  gsl_vector_set(ds.get(), 0, values_[recombination_key_ + "1"]);
  gsl_vector_set(ds.get(), 1, values_[recombination_key_ + "2"]);
  gsl_vector_set(ds.get(), 2, values_[recombination_key_ + "3"]);

  shared_ptr<const gsl_vector> ds_const = ds;
  shared_ptr<double> result( new double );

  gsl_blas_ddot(ds_const.get(), recombination_vercor_.get(), result.get());

  values_[recombination_key_] = *result;

  if(circut.empty()) {
    const string key = recombination_key_ + boost::lexical_cast<string>(id_);
    input_peer_->recombination_key_ = recombination_key_;
    input_peer_->publish(key, *result);
  } else {
    circut.push_back(recombination_key_);
    execute(circut);
  }

}

#endif