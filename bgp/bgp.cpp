#include <bgp/bgp.hpp>
#include <bgp/common.hpp>
#include <bgp/vertex.hpp>
#include <bgp/edge.hpp>
#include <peer/comp_peer.hpp>

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

    if (vertex < VERTEX_START) continue;
    if (vertex > VERTEX_END) continue;
    if (vertex == dst_vertex) continue;

    batch.push_back(vertex);
  }

  shared_ptr< pair<size_t, size_t> > counts_ptr(new pair<size_t, size_t>);

  for(auto& vertex: batch) {
      const auto functor = boost::bind(
          &BGPProcess::process_neighbors_mpc,
          this, vertex,
          changed_set_ptr,
          new_changed_set_ptr,
          counts_ptr);

      execution_stack_.push(functor);
  }

  continuation_ = boost::bind(
      &BGPProcess::next_iteration_finish,
      this, dst_vertex, new_changed_set_ptr);

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

  vector<vertex_t>& batch = *batch_ptr;
  shared_ptr< pair<size_t, size_t> > counts_ptr(new pair<size_t, size_t>);

  counts_ptr->second = execution_stack_.unsafe_size();

  size_t& count = counts_ptr->first;
  count = 0;

  vector<vertex_t> current_batch;

  for(;;) {
    if (execution_stack_.empty()) break;
    if (current_batch.size() == TASK_COUNT) break;

    boost::function<void()> functor;
    const bool popped = execution_stack_.try_pop(functor);\
    if (!popped) {
      std::cout << "crap!" << std::endl;
    }
    assert(popped);
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



void BGPProcess::process_neighbors_mpc(
    const vertex_t affected_vertex,
    shared_ptr< tbb::concurrent_unordered_set<vertex_t> > changed_set_ptr,
    shared_ptr< tbb::concurrent_unordered_set<vertex_t> > new_changed_set_ptr,
    shared_ptr< pair<size_t, size_t> > counts_ptr) {

  tbb::concurrent_unordered_set<vertex_t>& changed_set = *changed_set_ptr;

  shared_ptr< vector<vertex_t> > intersection_ptr(new vector<vertex_t>);
  vector<vertex_t>& intersection = *intersection_ptr;

  Vertex& affected = graph_[affected_vertex];
  auto& neighs = affected.neigh_;

  auto ch = vector<vertex_t>( changed_set.begin() , changed_set.end() );
  std::sort(ch.begin(), ch.end());

  std::set_intersection( neighs.begin(), neighs.end(), ch.begin(), ch.end(),
      std::insert_iterator< std::vector<vertex_t> >( intersection, intersection.begin() ) );

  if (intersection.size() < MAX_BATCH) {
    for0(affected_vertex, new_changed_set_ptr, counts_ptr, intersection_ptr);
  } else {

    shared_ptr< pair<size_t, size_t> > suncounter_ptr(new pair<size_t, size_t>);
    suncounter_ptr->second = intersection.size() / MAX_BATCH + 1;

    deque< typename vector<vertex_t>::iterator > range_stack;

    LOG4CXX_INFO(comp_peer_->logger_, "intersection.size() " << intersection.size()
        << " suncounter_ptr->second " << suncounter_ptr->second);

    for(size_t index = 0; index <= intersection.size() / MAX_BATCH; index++) {
      const size_t offset = MAX_BATCH * index;
      range_stack.push_back( intersection.begin() + offset );
    }
    range_stack.push_back( intersection.end() );

    shared_ptr< tbb::concurrent_vector<vertex_t> > local_set_ptr(
        new tbb::concurrent_vector<vertex_t>());


    while(range_stack.size() > 1) {

      auto start = range_stack.front();
      range_stack.pop_front();
      auto end = range_stack.front();
      auto pair = std::make_pair(start, end);

      shared_ptr< vertex_t > largest_vertex(new vertex_t(affected.next_hop_));

      compute_partial0(
          affected_vertex, largest_vertex, new_changed_set_ptr, local_set_ptr,
          counts_ptr, suncounter_ptr, intersection_ptr, pair);
    }

  }

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

  vector<vertex_t>& intersection = *intersection_ptr;

  vertex_t& largest_vertex = *largest_vertex_ptr;
  Vertex& affected = graph_[affected_vertex];

  size_t& count = global_counter_ptr->first;
  size_t& batch_count = global_counter_ptr->second;

  size_t& partial_count = local_counter_ptr->first;
  size_t& partial_batch_count = local_counter_ptr->second;

  const bool is_end =  (iter_range.first == iter_range.second);

  auto& local_set = *local_set_ptr;

  if (is_end) {

    m_.lock();
    if (largest_vertex != affected.next_hop_) {
      local_set.push_back(largest_vertex);
    }
    partial_count++;

    if (partial_batch_count == 1) {
      if (partial_count == partial_batch_count) {
        affected.set_next_hop(graph_, largest_vertex);
        count++;
        const bool cond = (count == batch_count);

        function<void()> functor;
        if (execution_stack_.try_pop(functor)) {
          m_.unlock();
          functor();
        } else {
          m_.unlock();
          if (cond) continuation_();
        }
        return;

      }

      m_.unlock();
      return;
    }

    if (partial_count == partial_batch_count) {
      m_.unlock();

      intersection.assign( local_set.begin(), local_set.end() );

      std::sort(intersection.begin(), intersection.end());

      LOG4CXX_TRACE(comp_peer_->logger_, "Final intersection size: " << intersection.size());

      partial_count = 0;
      partial_batch_count = 1;


      auto new_pair = std::make_pair(intersection.begin(), intersection.end());

      largest_vertex = affected.next_hop_;

      compute_partial0(
          affected_vertex, largest_vertex_ptr, new_changed_set_ptr, local_set_ptr,
          global_counter_ptr, local_counter_ptr, intersection_ptr, new_pair);

      return;
    }

    m_.unlock();
    return;
  }

  const vertex_t neigh_vertex = *(iter_range.first);
  iter_range.first++;

  const string key1 = lexical_cast<string>(largest_vertex);
  const string key2 = lexical_cast<string>(neigh_vertex);

  string w = ".2" + key1;
  string x = ".2" + key2;
  string y = ".2" + key1 + "-" + key2;

  string xy = y + "*" + x;
  string wx = x + "*" + w;
  string wy = y + "*" + w;

  string wxy2 = xy + "*" + "2" + "*" + w;
  string result = wx + "+" + wy + "-" + wxy2 + "-" + y + "-" + x + "+" + xy;

  affected.sig_bgp_next[result] =
      shared_ptr<boost::function<void()> >(new boost::function<void()>);

  auto next = boost::bind(&BGPProcess::compute_partial0, this,
        affected_vertex,
        largest_vertex_ptr,
        new_changed_set_ptr,
        local_set_ptr,
        global_counter_ptr,
        local_counter_ptr,
        intersection_ptr,
        iter_range
      );

  *(affected.sig_bgp_next[result]) = next;

  LOG4CXX_DEBUG(comp_peer_->logger_, "Vertex -> "
      << affected_vertex << ", " << neigh_vertex );

  affected.sig_bgp_cnt[result] = shared_ptr<boost::function<void(int)> >(new boost::function<void(int)>);

  *(affected.sig_bgp_cnt[result]) = boost::bind(
              &BGPProcess::compute_partial1, this,
              affected_vertex,
              neigh_vertex,
              largest_vertex_ptr,
              new_changed_set_ptr,
              local_set_ptr,
              _1);


  comp_peer_->compare0(key1, key2, affected_vertex);
}




void BGPProcess::compute_partial1(
    vertex_t affected_vertex,
    vertex_t neigh_vertex,
    shared_ptr< vertex_t > largest_vertex_ptr,
    shared_ptr< tbb::concurrent_unordered_set<vertex_t> > new_changed_set_ptr,
    shared_ptr< tbb::concurrent_vector<vertex_t> > local_set_ptr,
    int cmp) {

  tbb::concurrent_unordered_set<vertex_t>& new_changed_set = *new_changed_set_ptr;
  Vertex& affected = graph_[affected_vertex];

  vertex_t& largest_vertex = *largest_vertex_ptr;

  const string key1 = lexical_cast<string>(largest_vertex);
  const string key2 = lexical_cast<string>(neigh_vertex);

  string w = ".2" + key1;
  string x = ".2" + key2;
  string y = ".2" + key1 + "-" + key2;

  string xy = y + "*" + x;
  string wx = x + "*" + w;
  string wy = y + "*" + w;

  string wxy2 = xy + "*" + "2" + "*" + w;
  string result = wx + "+" + wy + "-" + wxy2 + "-" + y + "-" + x + "+" + xy;

  auto current_preference_it = affected.preference_.find(largest_vertex);
  BOOST_ASSERT(current_preference_it != affected.preference_.end());

  auto offer_it = affected.preference_.find(neigh_vertex);
  BOOST_ASSERT(offer_it != affected.preference_.end());

  const auto current_preference = current_preference_it->second;
  const auto offered_preference = offer_it->second;

  LOG4CXX_DEBUG(comp_peer_->logger_, "Compare -> "
      << current_preference << ", " << offered_preference );



  const bool condition = offered_preference <= current_preference;

  if (cmp != condition) {

    LOG4CXX_FATAL(comp_peer_->logger_, "==================================================");
    LOG4CXX_FATAL(comp_peer_->logger_,
        "(Is, Should): " <<
        "(" << cmp << ", " << condition << ") -- " <<
        "(" << affected_vertex << ", " << neigh_vertex << ")");
    LOG4CXX_FATAL(comp_peer_->logger_, "==================================================");

  }

  if ( offered_preference <= current_preference ) {
    affected.sig_bgp_next[result]->operator()();
    return;
  }

  largest_vertex = neigh_vertex;
  new_changed_set.insert(affected_vertex);

  affected.sig_bgp_next[result]->operator ()();
}




void BGPProcess::for0(
    const vertex_t affected_vertex,
    shared_ptr< tbb::concurrent_unordered_set<vertex_t> > new_changed_set_ptr,
    shared_ptr< pair<size_t, size_t> > counts_ptr,
    shared_ptr< vector<vertex_t> > intersection_ptr) {

  vector<vertex_t>& intersection = *intersection_ptr;
  size_t& count = counts_ptr->first;
  size_t& batch_count = counts_ptr->second;

  if (intersection.empty()) {

    m_.lock();
    count++;
    const bool cond = (count == batch_count);

    function<void()> functor;
    if (execution_stack_.try_pop(functor)) {
      m_.unlock();
      functor();
    } else {
      m_.unlock();
      if (cond) continuation_();
    }
    return;

    m_.unlock();
    return;
  }

  Vertex& affected = graph_[affected_vertex];
  const vertex_t neigh_vertex = intersection.back();
  intersection.pop_back();

  const string key1 = lexical_cast<string>(affected.next_hop_);
  const string key2 = lexical_cast<string>(neigh_vertex);

  string w = ".2" + key1;
  string x = ".2" + key2;
  string y = ".2" + key1 + "-" + key2;

  string xy = y + "*" + x;
  string wx = x + "*" + w;
  string wy = y + "*" + w;

  string wxy2 = xy + "*" + "2" + "*" + w;
  string result = wx + "+" + wy + "-" + wxy2 + "-" + y + "-" + x + "+" + xy;

  affected.sig_bgp_next[result] =
      shared_ptr<boost::function<void()> >(new boost::function<void()>);

  auto next = boost::bind(&BGPProcess::for0, this,
        affected_vertex,
        new_changed_set_ptr,
        counts_ptr,
        intersection_ptr
      );

  *(affected.sig_bgp_next[result]) = next;

  LOG4CXX_DEBUG(comp_peer_->logger_, "Vertex -> "
      << affected_vertex << ", " << neigh_vertex );

  affected.sig_bgp_cnt[result] = shared_ptr<boost::function<void(int)> >(new boost::function<void(int)>);

  *(affected.sig_bgp_cnt[result]) = boost::bind(&BGPProcess::for1, this,
              affected_vertex,
              neigh_vertex,
              new_changed_set_ptr,
              _1);


  comp_peer_->compare0(
      lexical_cast<string>(affected.next_hop_),
      lexical_cast<string>(neigh_vertex),
      affected_vertex);
}



void BGPProcess::for1(
    vertex_t affected_vertex,
    vertex_t neigh_vertex,
    shared_ptr< tbb::concurrent_unordered_set<vertex_t> > new_changed_set_ptr,
    int cmp) {

  tbb::concurrent_unordered_set<vertex_t>& new_changed_set = *new_changed_set_ptr;
  Vertex& affected = graph_[affected_vertex];

  const string key1 = lexical_cast<string>(affected.next_hop_);
  const string key2 = lexical_cast<string>(neigh_vertex);

  string w = ".2" + key1;
  string x = ".2" + key2;
  string y = ".2" + key1 + "-" + key2;

  string xy = y + "*" + x;
  string wx = x + "*" + w;
  string wy = y + "*" + w;

  string wxy2 = xy + "*" + "2" + "*" + w;
  string result = wx + "+" + wy + "-" + wxy2 + "-" + y + "-" + x + "+" + xy;

  const auto current_preference = affected.current_next_hop_preference(graph_);

  auto offer_it = affected.preference_.find(neigh_vertex);
  BOOST_ASSERT(offer_it != affected.preference_.end());
  const auto offered_preference = offer_it->second;

  LOG4CXX_DEBUG(comp_peer_->logger_, "Compare -> "
      << current_preference << ", " << offered_preference );



  const bool condition = offered_preference <= current_preference;

  if (cmp != condition) {

    LOG4CXX_FATAL(comp_peer_->logger_, "==================================================");
    LOG4CXX_FATAL(comp_peer_->logger_,
        "(Is, Should): " <<
        "(" << cmp << ", " << condition << ") -- " <<
        "(" << affected_vertex << ", " << neigh_vertex << ")");
    LOG4CXX_FATAL(comp_peer_->logger_, "==================================================");

  }

  if ( offered_preference <= current_preference ) {
    affected.sig_bgp_next[result]->operator()();
    return;
  }

  affected.set_next_hop(graph_, neigh_vertex);
  new_changed_set.insert(affected_vertex);

  affected.sig_bgp_next[result]->operator ()();
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

    Vertex& dstV = graph[dst];
    dstV.preference_setup_[dstRel].insert(src);

    boost::add_edge(src, dst, graph);
  }

  for(vertex_t v = 0; v < GRAPH_SIZE; v++) {

    size_t counter = 0;
    Vertex& vV = graph[v];
    vV.id_ = v;
    for(size_t i = 0; i < 3; i++) {
      auto& s = vV.preference_setup_[i];

      for(auto neigh: s) {
        vV.preference_[neigh] = counter;
        counter++;
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
