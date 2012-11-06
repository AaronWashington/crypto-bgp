  #ifndef INPUT_PEER_TEMPLATE_HPP_
#define INPUT_PEER_TEMPLATE_HPP_


#include <common.hpp>
#include <peer/input_peer.hpp>


typedef Secret<plaintext_t, COMP_PEER_NUM> secret_t;


template<size_t Num>
void Input_peer::result() {
  condition_.wait(lock_);

  double x[Num], y[Num], d[Num];

  print_values();

  for(auto i = 0; i < Num; i++) {

    std::string key = recombination_key_ + boost::lexical_cast<std::string>(i + 1);
    const auto value = values_[key];
    intermediary_[i + 1] = value;

  }

  for(auto it = intermediary_.begin(); it != intermediary_.end(); ++it) {
    const auto _x = it->first;
    const auto _y = it->second;
    const auto _index = it->first - 1;

    x[_index] = _x;
    y[_index] = _y;
  }

  gsl_poly_dd_init( d, x, y, 3 );
  auto aa = gsl_poly_dd_eval( d, x, 3, 0);

  LOG4CXX_INFO(logger_, "Result: " << aa);
}



template<class PlaintextMap, class CompPeerSeq>
void Input_peer::distribute_secrets(
    const PlaintextMap& secret_map,
    CompPeerSeq& comp_peers) {

  for(auto secret_pair : secret_map) {

    const symbol_t symbol = secret_pair.first;
    const plaintext_t value = secret_pair.second;

    secret_t secret(value);
    auto shares = secret.share();
    distribute_shares(symbol, shares, comp_peers);
  }

}



#endif /* INPUT_PEER_TEMPLATE_HPP_ */