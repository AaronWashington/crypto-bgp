#include <bgp/bgp.hpp>
#include <bgp/common.hpp>
#include <bgp/vertex.hpp>
#include <bgp/edge.hpp>
#include <peer/comp_peer.hpp>
#include <deque>

#include <algorithm>

BGPProcess::BGPProcess(
    string path,
    CompPeer<3> * comp_peer,
    io_service& io):
    graph_(GRAPH_SIZE),
    comp_peer_(comp_peer),
    io_service_(io) {

  load_graph(path, graph_);
  init(graph_);

}



void BGPProcess::start_callback(function<bool()> f) {

  start(graph_);
  end_ = f;

}



void BGPProcess::init(graph_t& graph) {

  auto iter = vertices(graph);
  auto last = iter.second;
  auto current = iter.first;

  for (; current != last; ++current) {
    const auto& current_vertex = *current;
    Vertex& vertex = graph[current_vertex];
    vertex.id_ = current_vertex;

    vertex.set_neighbors(graph);
    vertex.set_preference();
  }

}



void BGPProcess::start(graph_t& graph) {

  vertex_t dst_vertex = 0;
  Vertex& dst = graph[dst_vertex];

  shared_ptr< set<vertex_t> > affected_ptr(new set<vertex_t>);
  shared_ptr< tbb::concurrent_unordered_set<vertex_t> > changed_ptr(
      new tbb::concurrent_unordered_set<vertex_t>);

  set<vertex_t>& affected = *(affected_ptr);
  tbb::concurrent_unordered_set<vertex_t>& changed = *changed_ptr;

  changed.insert(dst_vertex);
  for(const vertex_t& vertex: dst.neigh_) {
    affected.insert(vertex);
  }

  next_iteration_start(dst_vertex, affected_ptr, changed_ptr);
}



void BGPProcess::next_iteration_start(
    const vertex_t dst_vertex,
    shared_ptr< set<vertex_t> > affected_set_ptr,
    shared_ptr< tbb::concurrent_unordered_set<vertex_t> > changed_set_ptr) {

  set<vertex_t>& affected_set = *(affected_set_ptr);
  tbb::concurrent_unordered_set<vertex_t>& changed_set = *changed_set_ptr;

  LOG4CXX_INFO(comp_peer_->logger_,
      "Next iteration... " << affected_set.size() << ": " << changed_set.size());

  shared_ptr<tbb::concurrent_unordered_set<vertex_t> > new_changed_set_ptr (
      new tbb::concurrent_unordered_set<vertex_t>);

  shared_ptr < vector<vertex_t> > batch_ptr(new vector<vertex_t>);
  vector<vertex_t>& batch = *batch_ptr;

  for(const auto vertex: affected_set) {
    batch.push_back(vertex);
  }

  next_iteration_continue(
    dst_vertex, batch_ptr, affected_set_ptr,
    changed_set_ptr, new_changed_set_ptr);
}



void BGPProcess::next_iteration_continue(
    const vertex_t dst_vertex,
    shared_ptr< vector<vertex_t> > batch_ptr,
    shared_ptr< set<vertex_t> > affected_set_ptr,
    shared_ptr< tbb::concurrent_unordered_set<vertex_t> > changed_set_ptr,
    shared_ptr< tbb::concurrent_unordered_set<vertex_t> > new_changed_set_ptr) {

  continuation_ = boost::bind(
        &BGPProcess::next_iteration_finish,
        this, dst_vertex, new_changed_set_ptr);

  vector<vertex_t>& batch = *batch_ptr;

  for(auto& vertex: batch) {

    shared_ptr< pair<size_t, size_t> > counts_ptr(new pair<size_t, size_t>);
    counts_ptr->first = 0;
    counts_ptr->second = batch.size() + 1;

    io_service_.post(
      boost::bind(
          &BGPProcess::process_neighbors_mpc,
          this, vertex,
          changed_set_ptr,
          new_changed_set_ptr,
          counts_ptr)
    );
  }

}


void BGPProcess::next_iteration_finish(
    const vertex_t dst_vertex,
    shared_ptr< tbb::concurrent_unordered_set<vertex_t> > new_changed_set_ptr) {

  tbb::concurrent_unordered_set<vertex_t>& new_changed_set = *new_changed_set_ptr;

  vector<vertex_t> nodes;

  for(const vertex_t vertex: new_changed_set) {
    nodes.push_back(vertex);
  }

  new_changed_set.clear();

  master_->sync(nodes);
  master_->barrier_->wait();

  for(size_t i = 0; i < master_->size_; i++) {
    new_changed_set.insert(master_->array_[i]);
  }

  shared_ptr<  set<vertex_t> > new_affected_set_ptr(new set<vertex_t>);
  set<vertex_t>& new_affected_set = *new_affected_set_ptr;

  for(const vertex_t vertex: new_changed_set) {
    auto neighbors = adjacent_vertices(vertex, graph_);
    new_affected_set.insert(neighbors.first, neighbors.second);
  }

  if(new_changed_set.empty())  {
    print_result();
    end_();
    return;
  }

  next_iteration_start(dst_vertex, new_affected_set_ptr, new_changed_set_ptr);
}



const string BGPProcess::get_recombination(vector<string>& circut) {
  return circut[2] + circut[0] + circut[1];
}



void BGPProcess::process_neighbors_mpc(
    const vertex_t affected_vertex,
    shared_ptr< tbb::concurrent_unordered_set<vertex_t> > changed_set_ptr,
    shared_ptr< tbb::concurrent_unordered_set<vertex_t> > new_changed_set_ptr,
    shared_ptr< pair<size_t, size_t> > counts_ptr) {

  tbb::concurrent_unordered_set<vertex_t>& changed_set = *changed_set_ptr;

  shared_ptr< vector<vertex_t> > intersection_ptr(new vector<vertex_t>);
  vector<vertex_t>& intersection = *intersection_ptr;

  Vertex& affected = graph_[affected_vertex];
  auto& vlm = affected.value_map_;
  auto& neighs = affected.neigh_;

  auto ch = vector<vertex_t>( changed_set.begin() , changed_set.end() );
  std::sort(ch.begin(), ch.end());

  shared_ptr< deque<pref_pair_t> > prefs_ptr(new deque<pref_pair_t>);
  std::deque<pref_pair_t>& prefs = *prefs_ptr;

  std::set_intersection( neighs.begin(), neighs.end(), ch.begin(), ch.end(),
      std::insert_iterator< std::vector<vertex_t> >( intersection, intersection.begin() ) );

  for(auto& neigh: intersection) {
    Vertex& offered = graph_[affected_vertex];
    const auto export_pair = std::make_pair(affected_vertex, affected.next_hop_);
    const auto pref = affected.preference_[neigh];
    const auto pref_pair = std::make_pair(neigh, pref);
    prefs.push_back(pref_pair);
  }

  std::sort(prefs.begin(), prefs.end(),
      boost::bind(&pref_pair_t::second, _1) >
      boost::bind(&pref_pair_t::second, _2)
  );

  for(auto& p: prefs) {
    LOG4CXX_INFO(comp_peer_->logger_,
        "State: " << affected_vertex << " | " << p.first << " | " << p.second);
  }


  vlm.clear();
  vlm["result"] = 0;
  vlm["acc0"] = 1;
  vlm["eql0"] = 1;
  vlm["neq0"] = 0;

  for0(affected_vertex, new_changed_set_ptr, counts_ptr, prefs_ptr);
}



void BGPProcess::for0(
    const vertex_t affected_vertex,
    shared_ptr< tbb::concurrent_unordered_set<vertex_t> > new_changed_set_ptr,
    shared_ptr< pair<size_t, size_t> > counts_ptr,
    shared_ptr< deque<pref_pair_t> > prefs_ptr) {

  deque<pref_pair_t>& prefs = *prefs_ptr;

  if (prefs.empty()) {
    for_distribute(affected_vertex, new_changed_set_ptr, counts_ptr, prefs_ptr);
    return;
  }

  size_t& count = counts_ptr->first;
  count++;

  const auto pref = prefs.front();
  prefs.pop_front();

  LOG4CXX_INFO(comp_peer_->logger_,
      "Preference: " << pref.first << " | " << pref.second);

  Vertex& affected = graph_[affected_vertex];
  auto& vlm = affected.value_map_;

  const string key = lexical_cast<string>(count);
  const string prev_key = lexical_cast<string>(count - 1);

  string val_key = "val" + key;
  string eql_key = "eql" + key;

  vector<string> circut;
  circut = {"==", "0", val_key};
  string for0_key = get_recombination(circut);

  string final_key = get_recombination(circut);

  LOG4CXX_INFO(comp_peer_->logger_,
      "Final key: " << final_key);

  affected.sig_bgp_next[final_key] =
      shared_ptr<boost::function<void()> >(new boost::function<void()>);

  *(affected.sig_bgp_next[final_key]) = boost::bind(
              &BGPProcess::for1, this,
              affected_vertex,
              new_changed_set_ptr,
              counts_ptr,
              prefs_ptr);

  vlm[val_key] = pref.first;
  vlm[eql_key] = 1;

  comp_peer_->execute(circut, affected_vertex);
}




void BGPProcess::for1(
    const vertex_t affected_vertex,
    shared_ptr< tbb::concurrent_unordered_set<vertex_t> > new_changed_set_ptr,
    shared_ptr< pair<size_t, size_t> > counts_ptr,
    shared_ptr< deque<pref_pair_t> > prefs_ptr) {

  size_t& count = counts_ptr->first;

  Vertex& affected = graph_[affected_vertex];
  auto& vlm = affected.value_map_;

  const string key = lexical_cast<string>(count);
  const string prev_key = lexical_cast<string>(count - 1);

  string val_key = "val" + key;
  string eql_key = "eql" + key;
  string neq_key = "neq" + key;

  vector<string> circut;
  circut = {"==", "0", val_key};
  string for0_key = get_recombination(circut);

  string pre_eql_key = "eql" + prev_key;
  string pre_acc_key = "acc" + prev_key;
  circut = {"*", pre_eql_key, pre_acc_key};

  string for1_key = get_recombination(circut);
  string final_key = for1_key;

  affected.sig_bgp_next[final_key] =
      shared_ptr<boost::function<void()> >(new boost::function<void()>);

  *(affected.sig_bgp_next[final_key]) = boost::bind(
              &BGPProcess::for2, this,
              affected_vertex,
              new_changed_set_ptr,
              counts_ptr,
              prefs_ptr);


  LOG4CXX_INFO(comp_peer_->logger_, "for0 " << vlm[for0_key]);

  vlm[eql_key] = vlm[for0_key];
  vlm[neq_key] = 1 - vlm[for0_key];
  comp_peer_->execute(circut, affected_vertex);

}


void BGPProcess::for2(
    const vertex_t affected_vertex,
    shared_ptr< tbb::concurrent_unordered_set<vertex_t> > new_changed_set_ptr,
    shared_ptr< pair<size_t, size_t> > counts_ptr,
    shared_ptr< deque<pref_pair_t> > prefs_ptr) {

  size_t& count = counts_ptr->first;

  Vertex& affected = graph_[affected_vertex];
  auto& vlm = affected.value_map_;

  const string key = lexical_cast<string>(count);
  const string prev_key = lexical_cast<string>(count - 1);

  string eql_key = "eql" + key;
  string neq_key = "neq" + key;

  vector<string> circut;
  circut = {"==", "0", eql_key};
  string for0_key = get_recombination(circut);

  string pre_eql_key = "eql" + prev_key;
  string pre_acc_key = "acc" + prev_key;
  circut = {"*", pre_eql_key, pre_acc_key};
  string for1_key = get_recombination(circut);

  string acc_key = "acc" + key;
  circut = {"*", neq_key, acc_key};
  string for2_key = get_recombination(circut);
  string final_key = for2_key;

  affected.sig_bgp_next[final_key] =
      shared_ptr<boost::function<void()> >(new boost::function<void()>);

  *(affected.sig_bgp_next[final_key]) = boost::bind(
              &BGPProcess::for3, this,
              affected_vertex,
              new_changed_set_ptr,
              counts_ptr,
              prefs_ptr);

  LOG4CXX_INFO(comp_peer_->logger_, "for1 " << vlm[for1_key]);
  vlm[acc_key] = vlm[for1_key];
  comp_peer_->execute(circut, affected_vertex);

}


void BGPProcess::for3(
    const vertex_t affected_vertex,
    shared_ptr< tbb::concurrent_unordered_set<vertex_t> > new_changed_set_ptr,
    shared_ptr< pair<size_t, size_t> > counts_ptr,
    shared_ptr< deque<pref_pair_t> > prefs_ptr) {


  size_t& count = counts_ptr->first;

  Vertex& affected = graph_[affected_vertex];
  auto& vlm = affected.value_map_;

  const string key = lexical_cast<string>(count);
  const string prev_key = lexical_cast<string>(count - 1);

  string eql_key = "eql" + key;
  string neq_key = "neq" + key;

  vector<string> circut;
  circut = {"==", "0", eql_key};
  string for0_key = get_recombination(circut);

  string pre_eql_key = "eql" + prev_key;
  string pre_acc_key = "acc" + prev_key;
  circut = {"*", pre_eql_key, pre_acc_key};
  string for1_key = get_recombination(circut);

  string acc_key = "acc" + key;
  circut = {"*", eql_key, acc_key};
  string for2_key = get_recombination(circut);

  string val_key = "val" + key;
  circut = {"*", for2_key, val_key};
  string for3_key = get_recombination(circut);

  string final_key = get_recombination(circut);

  affected.sig_bgp_next[final_key] =
      shared_ptr<boost::function<void()> >(new boost::function<void()>);

  *(affected.sig_bgp_next[final_key]) = boost::bind(
              &BGPProcess::for_add, this,
              affected_vertex,
              new_changed_set_ptr,
              counts_ptr,
              prefs_ptr);

  LOG4CXX_INFO(comp_peer_->logger_, "for2 " << vlm[for2_key]);
  comp_peer_->execute(circut, affected_vertex);
}



void BGPProcess::for_add(
    const vertex_t affected_vertex,
    shared_ptr< tbb::concurrent_unordered_set<vertex_t> > new_changed_set_ptr,
    shared_ptr< pair<size_t, size_t> > counts_ptr,
    shared_ptr< deque<pref_pair_t> > prefs_ptr) {

  size_t& count = counts_ptr->first;

  Vertex& affected = graph_[affected_vertex];
  auto& vlm = affected.value_map_;

  const string key = lexical_cast<string>(count);
  const string prev_key = lexical_cast<string>(count - 1);

  string eql_key = "eql" + key;
  string neq_key = "neq" + key;

  vector<string> circut;
  circut = {"==", "0", eql_key};
  string for0_key = get_recombination(circut);

  string pre_eql_key = "eql" + prev_key;
  string pre_acc_key = "acc" + prev_key;
  circut = {"*", pre_eql_key, pre_acc_key};
  string for1_key = get_recombination(circut);

  string acc_key = "acc" + key;
  circut = {"*", eql_key, acc_key};
  string for2_key = get_recombination(circut);

  string val_key = "val" + key;
  circut = {"*", for2_key, val_key};
  string for3_key = get_recombination(circut);

  string final_key = get_recombination(circut);

  LOG4CXX_INFO(comp_peer_->logger_, "for3 " << vlm[for3_key]);
  vlm["result"] = vlm["result"] + vlm[final_key];
  for0(affected_vertex, new_changed_set_ptr, counts_ptr, prefs_ptr);
}


void BGPProcess::for_distribute(
    const vertex_t affected_vertex,
    shared_ptr< tbb::concurrent_unordered_set<vertex_t> > new_changed_set_ptr,
    shared_ptr< pair<size_t, size_t> > counts_ptr,
    shared_ptr< deque<pref_pair_t> > prefs_ptr) {

  size_t& count = counts_ptr->first;

  Vertex& affected = graph_[affected_vertex];
  auto& vlm = affected.value_map_;

  const string key = lexical_cast<string>(count);
  const string prev_key = lexical_cast<string>(count - 1);

  string eql_key = "eql" + key;
  string neq_key = "neq" + key;

  vector<string> circut;
  circut = {"==", "0", eql_key};
  string for0_key = get_recombination(circut);

  string pre_eql_key = "eql" + prev_key;
  string pre_acc_key = "acc" + prev_key;
  circut = {"*", pre_eql_key, pre_acc_key};
  string for1_key = get_recombination(circut);

  string acc_key = "acc" + key;
  circut = {"*", eql_key, acc_key};
  string for2_key = get_recombination(circut);

  string val_key = "val" + key;
  circut = {"*", for2_key, val_key};
  string for3_key = get_recombination(circut);

  string final_key = get_recombination(circut);

  string result_string = "result";
  const auto value = vlm[result_string];


  affected.sig_bgp_next[final_key] =
      shared_ptr<boost::function<void()> >(new boost::function<void()>);

  *(affected.sig_bgp_next[final_key]) = boost::bind(
              &BGPProcess::for_final, this,
              affected_vertex,
              new_changed_set_ptr,
              counts_ptr,
              prefs_ptr);

  comp_peer_->distribute(result_string, value, affected_vertex);

}



void BGPProcess::for_final(
    const vertex_t affected_vertex,
    shared_ptr< tbb::concurrent_unordered_set<vertex_t> > new_changed_set_ptr,
    shared_ptr< pair<size_t, size_t> > counts_ptr,
    shared_ptr< deque<pref_pair_t> > prefs_ptr) {

  size_t& count = counts_ptr->first;

  Vertex& affected = graph_[affected_vertex];
  auto& vlm = affected.value_map_;

  const string key = lexical_cast<string>(count);
  const string prev_key = lexical_cast<string>(count - 1);

  string result_string = "result";
  const auto value = vlm[result_string];

  LOG4CXX_INFO(comp_peer_->logger_, "result " << value);

  if (value != affected.next_hop_) {
    affected.next_hop_ = value;
    auto& new_changed_set = *new_changed_set_ptr;
    new_changed_set.insert(affected_vertex);
  }

  continuation_();

}



void BGPProcess::compute_partial0(
    const vertex_t affected_vertex,
    shared_ptr< vertex_t > largest_vertex_ptr,
    shared_ptr< tbb::concurrent_unordered_set<vertex_t> > new_changed_set_ptr,
    shared_ptr< tbb::concurrent_vector<vertex_t> > local_set_ptr,
    shared_ptr< pair<size_t, size_t> > global_counter_ptr,
    shared_ptr< pair<size_t, size_t> > local_counter_ptr,
    shared_ptr< vector<vertex_t> > intersection_ptr,
    pair<vector<vertex_t>::iterator, vector<vertex_t>::iterator> iter_range) {
}




void BGPProcess::compute_partial1(
    vertex_t affected_vertex,
    vertex_t neigh_vertex,
    shared_ptr< vertex_t > largest_vertex_ptr,
    shared_ptr< tbb::concurrent_unordered_set<vertex_t> > new_changed_set_ptr,
    shared_ptr< tbb::concurrent_vector<vertex_t> > local_set_ptr,
    int cmp) {
}




#include <boost/algorithm/string.hpp>

void BGPProcess::load_graph(string path, graph_t& graph) {

  dynamic_properties dp;
  std::ifstream file(path);
  string s;

  while(true) {
    if( file.eof() ) break;
    getline(file, s);

    vector<string> tokens;
    boost::split(tokens, s, boost::is_any_of(" -;"));

    for (string token: tokens) {
      boost::algorithm::trim(token);
    }

    if(tokens.size() != 3) continue;

    vertex_t src = lexical_cast<size_t>(tokens[0]);
    vertex_t dst = lexical_cast<size_t>(tokens[1]);

    size_t srcRel = lexical_cast<size_t>(tokens[2]);
    size_t dstRel = 2 - srcRel;

    Vertex& srcV = graph[src];
    srcV.preference_setup_[srcRel].insert(dst);
    srcV.relationship_[dst] = srcRel;

    Vertex& dstV = graph[dst];
    dstV.preference_setup_[dstRel].insert(src);
    srcV.relationship_[src] = dstRel;

    boost::add_edge(src, dst, graph);
  }

  for(vertex_t v = 0; v < GRAPH_SIZE; v++) {

    size_t counter = 1;
    Vertex& vV = graph[v];
    vV.id_ = v;
    for(size_t i = 0; i < 3; i++) {
      auto& s = vV.preference_setup_[i];

      for(auto neigh: s) {
    	do {counter++;} while (counter % PRIME_EQ == 0);
        vV.preference_[neigh] = counter;

      }
    }

  }


  for(vertex_t v = 0; v < GRAPH_SIZE; v++) {
    Vertex& vV = graph[v];
    vV.set_neighbors(graph);
    vV.set_preference();
  }

}

void BGPProcess::load_graph2(string path, graph_t& graph) {

  std::ifstream file(path);
  dynamic_properties dp;

  dp.property("node_id", get(&Vertex::id_, graph));

  read_graphviz(file ,graph, dp, "node_id");
}



void BGPProcess::print_state(graph_t& graph) {
}



void BGPProcess::print_state(
    graph_t& graph,
    set<vertex_t>& affected_set,
    set<vertex_t>& changed_set) {

  std::cout << "Changed: ";
  for(auto a: affected_set) {
    std::cout << a << " ";
  }

  std::cout << std::endl;;

  std::cout << "Affected: ";
  for(auto a: changed_set) {
    std::cout << a << " ";
  }

  std::cout << std::endl;
}



void BGPProcess::print_result() {

  auto iter = vertices(graph_);
  auto last = iter.second;
  auto current = iter.first;

  LOG4CXX_FATAL(comp_peer_->logger_, "digraph G {");

  for (; current != last; ++current) {
    const auto& current_vertex = *current;
    Vertex& vertex = graph_[current_vertex];
    LOG4CXX_FATAL(comp_peer_->logger_, vertex.id_ << " -> " << vertex.next_hop_);
  }

  LOG4CXX_FATAL(comp_peer_->logger_, "}");

}
