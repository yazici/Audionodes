#include "audionodes.hpp"

#include "oscillator.hpp"
#include "sink.hpp"
#include "math.hpp"
const static std::map<std::string, NodeCreator> node_types = {
  NodeType(Oscillator, "OscillatorNode"),
  NodeType(Sink, "SinkNode"),
  NodeType(Math, "MathNode")
};

// Nodes addressed by unique integers
std::map<node_uid, Node*> node_storage;
node_uid node_storage_alloc() {
  if (node_storage.size()) {
    return node_storage.rbegin()->first+1;
  } else {
    return 0;
  }
}

NodeTree *main_node_tree;

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
  const static Sint16 maximum_value = (1 << 15)-1;
  const static Sint16 minimum_value = -(1 << 15);
  Chunk result = main_node_tree->evaluate();
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

// Methods to be used through the FFI
extern "C" {
  void initialize() {
    SDL_Init(SDL_INIT_AUDIO);

		SDL_AudioSpec spec;
		spec.freq     = rate;
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

  void cleanup() {
    SDL_CloseAudioDevice(dev);
    for (auto &id_node_pair : node_storage) {
      delete id_node_pair.second;
    }
    node_storage.clear();
    delete main_node_tree;
  }

  node_uid create_node(const char* type_c) {
    const std::string type = type_c;
    node_uid id = node_storage_alloc();
    if (node_types.count(type)) {
      Node *node = node_types.at(type)();
      node_storage[id] = node;
      return id;
    } else {
      std::cerr << "Audionodes native: Tried to create invalid node type" << std::endl;
      return -1;
    }
  }

  node_uid copy_node(node_uid old_id, const char* type_c) {
    node_uid new_id = create_node(type_c);
    if (new_id == -1) return -1;
    node_storage[new_id]->copy_input_values(*node_storage[old_id]);
    return new_id;
  }

  void remove_node(node_uid id) {
    if (!node_storage.count(id)) {
      std::cerr << "Audionodes native: Tried to remove non-existent node " << id << std::endl;
      return;
    }
    node_storage[id]->mark_deletion = true;
  }

  void update_node_input_value(node_uid id, int input_index, float value) {
    if (!node_storage.count(id)) {
      std::cerr << "Audionodes native: Tried to update input value of non-existent node " << id << std::endl;
      return;
    }
    node_storage[id]->set_input_value(input_index, value);
  }

  void update_node_property_value(node_uid id, int enum_index, int value) {
    if (!node_storage.count(id)) {
      std::cerr << "Audionodes native: Tried to update property value of non-existent node " << id << std::endl;
      return;
    }
    node_storage[id]->set_property_value(enum_index, value);
  }

  std::vector<NodeTree::ConstructionLink>* begin_tree_update() {
    std::vector<NodeTree::ConstructionLink> *links;
    links = new std::vector<NodeTree::ConstructionLink>();
    return links;
  }

  void add_tree_update_link(std::vector<NodeTree::ConstructionLink> *links, node_uid from_node, node_uid to_node, size_t from_socket, size_t to_socket) {
    if (!node_storage.count(from_node) || !node_storage.count(to_node)) {
      std::cerr << "Audionodes native: Tried to create a link to/from non-existent node " << from_node << " " << to_node << std::endl;
    }
    links->push_back({from_node, to_node, from_socket, to_socket});
  }

  void finish_tree_update(std::vector<NodeTree::ConstructionLink> *links) {
    std::map<node_uid, std::vector<NodeTree::ConstructionLink>> links_to;
    std::map<node_uid, int> links_from_count;
    for (auto link : *links) {
      links_to[link.to_node].push_back(link);
    }
    // Breadth first search from sinks to find reverse topological order
    std::vector<node_uid> q;
    std::vector<node_uid> marked_for_deletion;
    for (auto &id_node_pair : node_storage) {
      if (id_node_pair.second->mark_deletion) {
        marked_for_deletion.push_back(id_node_pair.first);
      } else if (id_node_pair.second->get_is_sink()) {
        q.push_back(id_node_pair.first);
        for (auto add_link : links_to[id_node_pair.first]) {
          links_from_count[add_link.from_node]++;
        }
      }
    }
    for (size_t i = 0; i < q.size(); ++i) {
      node_uid id = q[i];
      for (auto link : links_to[id]) {
        if (node_storage[link.from_node]->mark_deletion) continue;
        links_from_count[link.from_node]--;
        if (links_from_count[link.from_node] == 0) {
          q.push_back(link.from_node);
          for (auto add_link : links_to[link.from_node]) {
            links_from_count[add_link.from_node]++;
          }
        }
      }
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
        final_links[i][link.to_socket] = NodeTree::Link(true, node_index[link.from_node], link.from_socket);
      }
    }

    // Make a new node tree with the newly acquired evaluation parameters
    NodeTree *new_node_tree = new NodeTree(final_order, final_links);
    NodeTree *old_node_tree = main_node_tree;
    // Substitute the active node tree safely
    SDL_LockAudioDevice(dev);
    main_node_tree = new_node_tree;
    SDL_UnlockAudioDevice(dev);
    delete old_node_tree;

    // Lastly, we clean up the removed nodes
    for (node_uid id : marked_for_deletion) {
      Node *node = node_storage[id];
      delete node;
      node_storage.erase(id);
    }

    delete links;
  }
}