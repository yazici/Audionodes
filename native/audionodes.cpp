#include "audionodes.hpp"

namespace audionodes {

// Each node registers itself here (refer to node.hpp, RegisterNodeType)
NodeTypeMap& get_node_types() {
  static NodeTypeMap node_types;
  return node_types;
}

// Nodes addressed by unique integers
std::map<node_uid, Node*> node_storage;
node_uid node_storage_counter = 0;
node_uid node_storage_alloc() {
  return node_storage_counter++;
}

NodeTree *main_node_tree;

Message::Message() {}
Message::Message(Node* node, size_t slot, float audio_input) :
    type(Type::audio_input), node(node), slot(slot), audio_input(audio_input)
{}
Message::Message(Node* node, size_t slot, int property) :
    type(Type::property), node(node), slot(slot), property(property)
{}
Message::Message(Node* node, size_t slot, int length, void *binary) :
    type(Type::binary), node(node), slot(slot), property(length), binary(binary)
{}

void Message::apply() {
  switch (type) {
    case Type::audio_input:
      node->set_input_value(slot, audio_input);
      break;
    case Type::property:
      node->set_property_value(slot, property);
      break;
    case Type::binary:
      node->receive_binary(slot, property, binary);
      break;
  }
}

CircularBuffer<Message, 256> msg_queue;

void send_message(Message msg) {
  if (msg.node->mark_connected) {
    // Node is connected and actively used by the execution thread, use thread-safe communication
    // Sleep until queue has room
    for (size_t rep = 0; rep < 10 && msg_queue.full(); ++rep) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1000*N/RATE+1));
    }
    // Fail
    if (msg_queue.full()) {
      std::cerr << "Audionodes native: Unable to communicate with execution thread" << std::endl;
      if (msg.type == Message::Type::binary) {
        char *ptr = (char*) msg.binary;
        delete [] ptr;
      }
      return;
    }
    msg_queue.push(msg);
  } else {
    // Apply the message directly
    msg.apply();
  }
}

void audio_callback(void *userdata, Uint8 *_stream, int len) {
  // Cast byte stream into 16-bit signed int stream
  Sint16 *stream = (Sint16*) _stream;
  len /= 2;
  if (len != N) {
    // Horrible failure
    std::cerr << "Audionodes native: Unexpected sample count: " << len << std::endl;
    return;
  }
  if (main_node_tree == nullptr) {
    for (int i = 0; i < len; ++i) stream[i] = 0;
    return;
  }
  while (!msg_queue.empty()) {
    Message msg = msg_queue.pop();
    msg.apply();
  }
  constexpr Sint16 maximum_value = (1 << 15)-1;
  constexpr Sint16 minimum_value = -(1 << 15);
  const Chunk &result = main_node_tree->evaluate();
  for (int i = 0; i < len; ++i) {
    if (result[i] < -1) {
      stream[i] = minimum_value;
    } else if (result[i] >= 1) {
      stream[i] = maximum_value;
    } else {
      stream[i] = result[i] * maximum_value;
    }
  }
}

SDL_AudioDeviceID dev;
bool initialized = false;

// Methods to be used through the FFI
extern "C" {
  void audionodes_register_node_type(const char *identifier, Node::Creator creator) {
    get_node_types()[identifier] = creator;
  }
  
  void audionodes_unregister_node_type(const char *identifier) {
    get_node_types().erase(identifier);
  }
  
  void audionodes_initialize() {
    SDL_Init(SDL_INIT_AUDIO);

    SDL_AudioSpec spec;
    spec.freq     = RATE;
    spec.format   = AUDIO_S16SYS;
    spec.channels = 1;
    spec.samples  = N;
    spec.callback = audio_callback;
    spec.userdata = nullptr;
    SDL_AudioSpec obtainedSpec;
    dev = SDL_OpenAudioDevice(NULL, 0, &spec, &obtainedSpec, 0);
    if (dev == 0) {
      std::cerr << "Audionides Native: Unable to open audio device: " << SDL_GetError() << std::endl;
      return;
    }
    if (obtainedSpec.samples == N/2) {
      // The sample rate gets halved on some systems for some reason
      // -> correct for that
      std::cerr << "Audionodes Native: SDL halved sample rate... trying to correct" << std::endl;
      SDL_CloseAudioDevice(dev);
      spec.samples = 2*N;
      obtainedSpec = SDL_AudioSpec();
      dev = SDL_OpenAudioDevice(NULL, 0, &spec, &obtainedSpec, 0);
      if (obtainedSpec.samples != N || dev == 0) {
        std::cerr << "Audionodes Native: Halving correction failed " << SDL_GetError() << std::endl;
        return;
      }
    }
    SDL_PauseAudioDevice(dev, 0);
  }

  void audionodes_cleanup() {
    SDL_CloseAudioDevice(dev);
    for (auto &id_node_pair : node_storage) {
      delete id_node_pair.second;
    }
    node_storage.clear();
    delete main_node_tree;
    main_node_tree = nullptr;
  }

  node_uid audionodes_create_node(const char* type) {
    node_uid id = node_storage_alloc();
    if (get_node_types().count(type)) {
      Node *node = get_node_types().at(type)();
      node_storage[id] = node;
      return id;
    } else {
      std::cerr << "Audionodes native: Tried to create node of invalid type \"" << type << "\"" << std::endl;
      return -1;
    }
  }

  node_uid audionodes_copy_node(node_uid old_id, const char* type_c) {
    node_uid new_id = audionodes_create_node(type_c);
    if (new_id == -1) return -1;
    node_storage[new_id]->copy_input_values(*node_storage[old_id]);
    return new_id;
  }

  void audionodes_remove_node(node_uid id) {
    if (!node_storage.count(id)) {
      std::cerr << "Audionodes native: Tried to remove non-existent node " << id << std::endl;
      return;
    }
    node_storage[id]->mark_deletion = true;
  }
  
  bool audionodes_node_exists(node_uid id) {
    return node_storage.count(id);
  }

  void audionodes_update_node_input_value(node_uid id, int input_index, float value) {
    if (!node_storage.count(id)) {
      std::cerr << "Audionodes native: Tried to update input value of non-existent node " << id << std::endl;
      return;
    }
    send_message(Message(node_storage[id], input_index, value));
  }

  void audionodes_update_node_property_value(node_uid id, int enum_index, int value) {
    if (!node_storage.count(id)) {
      std::cerr << "Audionodes native: Tried to update property value of non-existent node " << id << std::endl;
      return;
    }
    send_message(Message(node_storage[id], enum_index, value));
  }
  
  void audionodes_send_node_binary_data(node_uid id, int slot, int length, void *_bin) {
    if (!node_storage.count(id)) {
      std::cerr << "Audionodes native: Tried to send binary data to non-existent node " << id << std::endl;
      return;
    }
    // Node (or send_message on failure) will handle deletion
    char *bin = new char[length];
    std::memcpy(bin, _bin, length);
    send_message(Message(node_storage[id], slot, length, bin));
  }

  std::vector<NodeTree::ConstructionLink>* audionodes_begin_tree_update() {
    std::vector<NodeTree::ConstructionLink> *links;
    links = new std::vector<NodeTree::ConstructionLink>();
    return links;
  }

  void audionodes_add_tree_update_link(std::vector<NodeTree::ConstructionLink> *links, node_uid from_node, node_uid to_node, size_t from_socket, size_t to_socket) {
    if (!node_storage.count(from_node) || !node_storage.count(to_node)) {
      std::cerr << "Audionodes native: Tried to create a link to/from non-existent node " << from_node << " " << to_node << std::endl;
    }
    links->push_back({from_node, to_node, from_socket, to_socket});
  }

  void audionodes_finish_tree_update(std::vector<NodeTree::ConstructionLink> *links) {
    std::map<node_uid, std::vector<NodeTree::ConstructionLink>> links_to;
    std::map<node_uid, int> links_from_count;
    for (auto link : *links) {
      links_to[link.to_node].push_back(link);
    }
    // Breadth first search from sinks to find reverse topological order
    std::vector<node_uid> q;
    std::vector<node_uid> marked_for_deletion;
    // will contain all nodes that need to be evaluated (directly or indirectly connected to a sink)
    std::set<node_uid> to_process;
    std::vector<node_uid> to_process_q;
    for (auto &id_node_pair : node_storage) {
      if (id_node_pair.second->mark_deletion) {
        marked_for_deletion.push_back(id_node_pair.first);
      } else {
        if (id_node_pair.second->get_is_sink()) {
          q.push_back(id_node_pair.first);
          to_process.insert(id_node_pair.first);
          to_process_q.push_back(id_node_pair.first);
        }
      }
    }
    for (size_t i = 0; i < to_process_q.size(); ++i) {
      node_uid id = to_process_q[i];
      for (auto link : links_to[id]) {
        if (node_storage[link.from_node]->mark_deletion) continue;
        links_from_count[link.from_node]++;
        if (to_process.count(link.from_node)) continue;
        to_process.insert(link.from_node);
        to_process_q.push_back(link.from_node);
      }
    }
    for (size_t i = 0; i < q.size(); ++i) {
      node_uid id = q[i];
      for (auto link : links_to[id]) {
        if (!to_process.count(link.from_node)) continue;
        links_from_count[link.from_node]--;
        if (links_from_count[link.from_node] == 0) {
          q.push_back(link.from_node);
        }
      }
    }
    if (q.size() < to_process.size()) {
      // Not all nodes that were supposed to be included got into the order ->
      // there is a loop
      std::cerr << "Audionodes Native: Error building tree: loop found" << std::endl;
      delete links;
      return;
    }
    
    // Reverse the resulting vector to have the correct evaluation order
    std::reverse(q.begin(), q.end());

    // Collect final evaluation order and links
    std::vector<Node*> final_order(q.size());
    std::vector<std::vector<NodeTree::Link>> final_links(q.size());
    std::map<node_uid, size_t> node_index;
    for (size_t i = 0; i < q.size(); ++i) {
      node_uid id = q[i];
      node_index[id] = i;
      Node *node = node_storage[id];
      final_order[i] = node;
      final_links[i] = std::vector<NodeTree::Link>(node->get_input_count());
      for (auto link : links_to[id]) {
        if (!to_process.count(link.from_node)) continue;
        final_links[i][link.to_socket] = NodeTree::Link(true, node_index[link.from_node], link.from_socket);
      }
    }

    // Call callbacks on newly connected nodes
    for (Node *node : final_order) {
      if (!node->mark_connected) {
        node->mark_connected = true;
        node->connect_callback();
      }
      node->_tmp_connected = true;
    }

    // Make a new node tree with the newly acquired evaluation parameters
    NodeTree *new_node_tree = new NodeTree(final_order, final_links);
    NodeTree *old_node_tree = main_node_tree;
    // Substitute the active node tree safely
    SDL_LockAudioDevice(dev);
    main_node_tree = new_node_tree;
    SDL_UnlockAudioDevice(dev);
    delete old_node_tree;

    // Call callbacks on newly disconnected nodes
    for (auto &id_node_pair : node_storage) {
      Node *node = id_node_pair.second;
      if (!node->_tmp_connected && node->mark_connected) {
        node->mark_connected = false;
        node->disconnect_callback();
      }
      node->_tmp_connected = false;
    }

    // Lastly, we clean up the removed nodes
    for (node_uid id : marked_for_deletion) {
      Node *node = node_storage[id];
      delete node;
      node_storage.erase(id);
    }

    delete links;
  }
}

}
