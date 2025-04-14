#define _Alignof(x) __alignof__(x)
#include <iostream>
#include <unordered_map>
#include <vector>
#include <unordered_set>
#include <pthread.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <algorithm>
#include <stdexcept>

#include <queue>


#include <sys/stat.h>
#include <unistd.h>    // For access()
#include <cerrno>      // For errno
#include <cstring>     // For strerror()

#include "utils.cpp"
#include "config.h"
#include "segment.h"
#include "messages/message.h"
#include "messages/node2tracker.h"
#include "messages/tracker2node.h"

/**
 * @struct pair_hash
 * @brief Custom hash function for std::pair to enable use in unordered containers
 * 
 * This is needed because the STL doesn't provide a default hash for pairs.
 */
struct pair_hash {
    /**
     * @brief Operator to compute hash of a pair
     * @tparam T1 Type of first element in pair
     * @tparam T2 Type of second element in pair
     * @param p The pair to hash
     * @return Combined hash value of the pair elements
     */
    template <typename T1, typename T2>
    std::size_t operator()(const std::pair<T1, T2>& p) const {
        std::size_t hash1 = std::hash<T1>{}(p.first);
        std::size_t hash2 = std::hash<T2>{}(p.second);
        return hash1 ^ (hash2 << 1); // Combine hashes with bit shifting
    }
};

/**
 * @brief Specialization of std::hash for pairs containing another pair
 * 
 * This enables using complex nested pairs as keys in unordered containers.
 */
template <>
struct std::hash<std::pair<int, std::pair<std::string, int>>> {
    /**
     * @brief Operator to compute hash of a nested pair structure
     * @param p The pair to hash (node_id, (ip, port))
     * @return Combined hash value of all elements
     */
    std::size_t operator()(const std::pair<int, std::pair<std::string, int>>& p) const {
        std::size_t h1 = std::hash<int>{}(p.first); // node_id
        std::size_t h2 = std::hash<std::string>{}(p.second.first); // ip
        std::size_t h3 = std::hash<int>{}(p.second.second); // port
        return h1 ^ (h2 << 1) ^ (h3 << 2); // Combine with bit shifting
    }
};

/**
 * @class Tracker
 * @brief Main tracker class that manages peer-to-peer file sharing network
 * 
 * The tracker maintains:
 * - List of files and their owners
 * - Node activity status
 * - Communication sockets
 * - Periodic node health checks
 */
class Tracker {
private:
    // Static members shared across all tracker instances
    static int tracker_socket; // UDP socket for communication
    static std::unordered_map<std::string, std::vector<FileOwner>> file_owners_list; // Maps filenames to their owners
    static std::unordered_map<int, int> send_freq_list; // Tracks how often each node shares files
    static std::unordered_map<std::pair<int, std::pair<std::string, int>>, bool> has_informed_tracker; // Node liveness tracking
    static pthread_mutex_t mutex; // Mutex for thread-safe operations


    static std::queue<std::pair<std::unordered_map<std::string, std::any>, sockaddr_in>> request_queue;
    static pthread_mutex_t queue_mutex;
    static pthread_cond_t queue_cond;
    static pthread_mutex_t log_mutex;  // Separate mutex for logging
    static pthread_mutex_t data_mutex; // Separate mutex for data structures

public:
    /**
     * @brief Constructor - initializes tracker socket and mutex
     * @throws std::runtime_error if socket creation fails
     */
    Tracker() {
        tracker_socket = set_socket(Config::Constants::TRACKER_PORT);
        if (tracker_socket < 0) {
            throw std::runtime_error("Failed to create tracker socket");
        }
        pthread_mutex_init(&mutex, nullptr);
        pthread_mutex_init(&queue_mutex, nullptr);
        pthread_mutex_init(&log_mutex, nullptr);
        pthread_mutex_init(&data_mutex, nullptr);
        pthread_cond_init(&queue_cond, nullptr);
    }

    /**
     * @brief Destructor - cleans up socket and mutex
     */
    ~Tracker() {
        close(tracker_socket);
        pthread_mutex_destroy(&mutex);
        pthread_mutex_destroy(&queue_mutex);
        pthread_mutex_destroy(&log_mutex);
        pthread_mutex_destroy(&data_mutex);
        pthread_cond_destroy(&queue_cond);
    }


    static void log_thread_safe(int node_id, const std::string& content, bool show) {
        pthread_mutex_lock(&log_mutex);
        log(node_id, content, show);
        pthread_mutex_unlock(&log_mutex);
    }

    /**
     * @brief Saves current tracker database to text files
     * 
     * Creates two files:
     * - nodes_Freq_list.txt: Node IDs and their file sharing frequency
     * - files_Owners_list.txt: Files and their owner nodes
     */
    static void save_db_as_txt() {
        std::string dir = Config::Directory::TRACKER_DB_DIR;
    
        // Ensure the directory exists (using stat instead of filesystem::exists)
        struct stat st;
        if (stat(dir.c_str(), &st) != 0) {
            // Directory doesn't exist, try to create it
            if (mkdir(dir.c_str(), 0755) != 0) {  // 0755 = rwxr-xr-x permissions
                std::cerr << "Error: Failed to create directory " << dir 
                          << ": " << strerror(errno) << "\n";
                return;
            }
        } else if (!S_ISDIR(st.st_mode)) {
            std::cerr << "Error: " << dir << " exists but is not a directory\n";
            return;
        }
    
        std::string nodes_info_path = dir + "nodes_Freq_list.txt";
        std::string files_info_path = dir + "files_Owners_list.txt";
    
        // Save nodes' information as a text file
        std::ofstream nodes_file(nodes_info_path);
        if (nodes_file.is_open()) {
            for (const auto& [node_id, freq] : send_freq_list) {
                nodes_file << "node" << node_id << " " << freq << "\n";
            }
            nodes_file.close();
        } else {
            std::cerr << "Error: Could not open " << nodes_info_path << " for writing.\n";
        }
    
        // Save files' information as a text file
        std::ofstream files_file(files_info_path);
        if (files_file.is_open()) {
            for (const auto& [filename, owners] : file_owners_list) {
                files_file << filename << " : ";
                for (const auto& owner : owners) {
                    files_file << "(" << owner.node_id << ", " << owner.addr.first << ", " << owner.addr.second << ") ";
                }
                files_file << "\n";
            }
            files_file.close();
        } else {
            std::cerr << "Error: Could not open " << files_info_path << " for writing.\n";
        }
    }

    /**
     * @brief Removes a node from all tracker records
     * @param node_id The ID of node to remove
     * @param ip IP address of the node
     * @param port Port number of the node
     */
    static void remove_node(int node_id, const std::string& ip, int port) {
        std::pair<int, std::pair<std::string, int>> entry = {node_id, {ip, port}};
    
        pthread_mutex_lock(&data_mutex);
    
        // Remove from send frequency and informed tracker
        send_freq_list.erase(node_id);
        has_informed_tracker.erase(entry);
    
        // Classic loop over map without structured bindings
        for (auto it = file_owners_list.begin(); it != file_owners_list.end(); ) {
            std::string filename = it->first;
            std::vector<FileOwner>& owners = it->second;
    
            // Remove all owners with the given node_id
            for (auto owner_it = owners.begin(); owner_it != owners.end(); ) {
                if (owner_it->node_id == node_id) {
                    owner_it = owners.erase(owner_it);
                } else {
                    ++owner_it;
                }
            }
    
            // Remove file entry if no owners left
            if (owners.empty()) {
                it = file_owners_list.erase(it);
            } else {
                ++it;
            }
        }
    
        pthread_mutex_unlock(&data_mutex);
        save_db_as_txt();
    }
    
    
    /**
     * @brief Periodically checks node liveness
     * @param arg Thread argument (unused)
     * @return nullptr (thread function requirement)
     * 
     * Runs in a separate thread to:
     * - Track which nodes have recently checked in
     * - Identify and remove dead nodes
     * - Log node status changes
     */
    static void* check_nodes_periodically(void* arg) {
        int interval = Config::Constants::TRACKER_TIME_INTERVAL;
    
        while (true) {
            std::unordered_set<int> alive_nodes_ids;
            std::unordered_set<int> dead_nodes_ids;
            std::vector<std::pair<int, std::pair<std::string, int>>> to_remove;
            std::vector<std::pair<std::pair<int, std::pair<std::string, int>>, bool>> to_update;
    
            pthread_mutex_lock(&data_mutex);
            for (const auto& it : has_informed_tracker) {
                int node_id = it.first.first;
                bool has_informed = it.second;
    
                if (has_informed) {
                    to_update.push_back({it.first, false});
                    alive_nodes_ids.insert(node_id);
                } else {
                    dead_nodes_ids.insert(node_id);
                    to_remove.push_back(it.first);
                }
            }
    
            for (const auto& entry : to_update) {
                has_informed_tracker[entry.first] = entry.second;
            }

            pthread_mutex_unlock(&data_mutex);

            for (const auto& entry : to_remove) {
                remove_node(entry.first, entry.second.first, entry.second.second);
            }

            if (!alive_nodes_ids.empty() || !dead_nodes_ids.empty()) {
                std::string log_content = "=== Node Status ===\nAlive: ";
                for (int id : alive_nodes_ids) log_content += std::to_string(id) + " ";
                log_content += "\nDead: ";
                for (int id : dead_nodes_ids) log_content += std::to_string(id) + " ";
                log(0, log_content, true);
            }
    
            sleep(interval);
        }
        return nullptr;
    }


    static void* request_dispatcher(void* arg) {
        while (true) {
            pthread_mutex_lock(&queue_mutex);
            while (request_queue.empty()) {
                pthread_cond_wait(&queue_cond, &queue_mutex);
            }
    
            auto request = request_queue.front();
            request_queue.pop();
            pthread_mutex_unlock(&queue_mutex);
    
            // Create a new thread to handle the request
            pthread_t thread;
            auto args = new std::pair<std::unordered_map<std::string, std::any>, sockaddr_in>(request);
            
            if (pthread_create(&thread, nullptr, [](void* arg) -> void* {
                auto* req = static_cast<std::pair<std::unordered_map<std::string, std::any>, sockaddr_in>*>(arg);
                try {
                    handle_node_request(req->first, req->second);
                } catch (const std::exception& e) {
                    log_thread_safe(0, "Error handling request: " + std::string(e.what()), true);
                }
                delete req;
                return nullptr;
            }, args) != 0) {
                log_thread_safe(0, "Error: Failed to create client handler thread", true);
                delete args;
            } else {
                pthread_detach(thread);
            }
        }
        return nullptr;
    }



    /**
     * @brief Listens for incoming node messages
     * @param arg Thread argument (unused)
     * @return nullptr (thread function requirement)
     * 
     * Main listener thread that:
     * - Starts the periodic node checker
     * - Receives UDP messages from nodes
     * - Spawns worker threads to handle each request
     */
    static void* listen(void* arg) {
        // Start periodic node health check thread
        pthread_t timer_thread;
        if (pthread_create(&timer_thread, nullptr, &Tracker::check_nodes_periodically, nullptr) != 0) {
            log_thread_safe(0, "Error: Failed to create timer thread", true);
            return nullptr;
        }
        pthread_detach(timer_thread);
    
        // Start request dispatcher thread
        pthread_t dispatcher_thread;
        if (pthread_create(&dispatcher_thread, nullptr, &Tracker::request_dispatcher, nullptr) != 0) {
            log_thread_safe(0, "Error: Failed to create dispatcher thread", true);
            return nullptr;
        }
        pthread_detach(dispatcher_thread);
    
        // Main receive loop
        while (true) {
            char buffer[Config::Constants::BUFFER_SIZE];
            struct sockaddr_in client_addr;
            socklen_t addr_len = sizeof(client_addr);
    
            ssize_t recv_len = recvfrom(tracker_socket, buffer, sizeof(buffer), 0,
                                       (struct sockaddr*)&client_addr, &addr_len);
            if (recv_len < 0) {
                log_thread_safe(0, "recvfrom failed", true);
                continue;
            }
    
            if (recv_len > 0) {
                try {
                    std::vector<char> received_data(buffer, buffer + recv_len);
                    auto properties = Message::decode(received_data);
    
                    // Add request to queue
                    pthread_mutex_lock(&queue_mutex);
                    request_queue.push({properties, client_addr});
                    pthread_cond_signal(&queue_cond);
                    pthread_mutex_unlock(&queue_mutex);
                } catch (const std::exception& e) {
                    log_thread_safe(0, "Error decoding message: " + std::string(e.what()), true);
                }
            }
        }
        return nullptr;
    }
    
    /**
     * @brief Handles decoded node requests
     * @param properties Decoded message properties
     * @param client_addr Node's address information
     * 
     * Routes requests based on message mode:
     * - REGISTER: Node joining network
     * - OWN: Node sharing a file
     * - NEED: Node requesting file locations
     * - UPDATE: Node updating its status
     * - EXIT: Node leaving network
     */
    static void handle_node_request(const std::unordered_map<std::string, std::any>& properties, const sockaddr_in& client_addr) {
        try {
            std::string ip_addr = inet_ntoa(client_addr.sin_addr);
            int port = ntohs(client_addr.sin_port);
            std::pair<std::string, int> addr = {ip_addr, port};
    
            int mode = std::stoi(std::any_cast<std::string>(properties.at("mode")));
            int node_id = std::stoi(std::any_cast<std::string>(properties.at("node_id")));
    
            switch (mode) {
                case 1: { // OWN
                    add_file_owner(properties, addr);
                    break;
                }
    
                case 2: { // NEED
                    search_file(properties, addr);
                    break;
                }
    
                case 3: { // UPDATE
                    update_db(properties);
                    break;
                }
    
                case 0: { // REGISTER
                    pthread_mutex_lock(&mutex);
                    has_informed_tracker[{node_id, addr}] = true;
                    pthread_mutex_unlock(&mutex);
    
                    std::string ack = "ACK";
                    sendto(tracker_socket, ack.c_str(), ack.size(), 0,
                        (const struct sockaddr*)&client_addr, sizeof(client_addr));
    
                    std::string log_content = "ACK sent to Node " + std::to_string(node_id) + " at " + addr.first + ":" + std::to_string(addr.second);
                    log(0, log_content, true);
                    break;
                }
    
                case 4: { // EXIT
                    remove_node(node_id, addr.first, addr.second);
                    std::string log_content = "Node " + std::to_string(node_id) + " exited the torrent intentionally.";
                    log(0, log_content, true);
                    break;
                }
    
                case 5: { // HEARTBEAT
                    pthread_mutex_lock(&mutex);
                    has_informed_tracker[{node_id, addr}] = true;
                    pthread_mutex_unlock(&mutex);
                    break;
                }
    
                default: {
                    std::cerr << "Error: Invalid mode " << mode << " received from node " << node_id << "\n";
                    break;
                }
            }
    
        } catch (const std::exception& e) {
            std::cerr << "Error handling node request: " << e.what() << "\n";
        }
    }

    /**
     * @brief Adds a file owner to the tracker database
     * @param properties Decoded message properties
     * @param addr Node's address information
     */
    static void add_file_owner(const std::unordered_map<std::string, std::any>& properties, const std::pair<std::string, int>& addr) {
        try {
            std::string filename = std::any_cast<std::string>(properties.at("filename"));
            int node_id = std::stoi(std::any_cast<std::string>(properties.at("node_id")));

            std::string log_content = "Node " + std::to_string(node_id) + " owns " + filename + " and is ready to send.";
            log(0, log_content, true);

            FileOwner owner = {node_id, addr};

            // Thread-safe addition to ownership list
            pthread_mutex_lock(&mutex);
            file_owners_list[filename].push_back(owner);
            pthread_mutex_unlock(&mutex);

            save_db_as_txt(); // Persist changes
        } catch (const std::exception& e) {
            std::cerr << "Error adding file owner: " << e.what() << "\n";
        }
    }

    /**
     * @brief Handles file search requests
     * @param properties Decoded message properties
     * @param addr Node's address information
     */
    static void search_file(const std::unordered_map<std::string, std::any>& properties, const std::pair<std::string, int>& addr) {
        try {
            std::string filename = std::any_cast<std::string>(properties.at("filename"));
            int node_id = std::stoi(std::any_cast<std::string>(properties.at("node_id")));

            std::string log_content = "Node " + std::to_string(node_id) + " is searching for " + filename;
            log(0, log_content, true);

            std::vector<std::pair<FileOwner, int>> search_result;

            // Thread-safe search of ownership list
            pthread_mutex_lock(&mutex);
            if (file_owners_list.find(filename) != file_owners_list.end()) {
                std::vector<FileOwner> nodes = file_owners_list[filename];
                for (const auto& owner : nodes) {
                    // Include each owner's sharing frequency in results
                    search_result.push_back({owner, send_freq_list[owner.node_id]});
                }
            }
            pthread_mutex_unlock(&mutex);

/////////////////////////////////////////////////////////////////
            for(const auto& owner : search_result) {
                std::cout << "Node ID: " << owner.first.node_id << ", Frequency: " << owner.second << "\n";
            }

            // Send response back to requesting node
            Tracker2Node response(node_id, search_result, filename);
            std::vector<char> response_data = response.encode();
            send_segment(tracker_socket, response_data, addr);
        } catch (const std::exception& e) {
            std::cerr << "Error searching file: " << e.what() << "\n";
        }
    }

    /**
     * @brief Sends data segment to a node
     * @param tracker_socket Socket to send from
     * @param data Data to send
     * @param addr Destination address
     */
    static void send_segment(int tracker_socket, const std::vector<char>& data, const std::pair<std::string, int>& addr) {
        try {
            // Create UDP segment wrapper
            UDPSegment segment(Config::Constants::TRACKER_PORT, addr.second, data);

            // Prepare destination address structure
            sockaddr_in client_addr;
            memset(&client_addr, 0, sizeof(client_addr));
            client_addr.sin_family = AF_INET;
            client_addr.sin_port = htons(addr.second);
            inet_pton(AF_INET, addr.first.c_str(), &client_addr.sin_addr);

            // Send raw data
            ssize_t sent_len = sendto(tracker_socket, data.data(), data.size(), 0,
                                      (struct sockaddr*)&client_addr, sizeof(client_addr));
            if (sent_len < 0) {
                perror("sendto failed");
            }
        } catch (const std::exception& e) {
            std::cerr << "Error sending segment: " << e.what() << "\n";
        }
    }

    /**
     * @brief Updates node sharing frequency
     * @param properties Decoded message properties
     */
    static void update_db(const std::unordered_map<std::string, std::any>& properties) {
        try {
            std::string filename = std::any_cast<std::string>(properties.at("filename"));
            int node_id = std::stoi(std::any_cast<std::string>(properties.at("node_id")));

            std::string log_content = "Node " + std::to_string(node_id) + " updated the file list for " + filename;
            log(0, log_content, true);

            // Thread-safe frequency update
            pthread_mutex_lock(&mutex);
            send_freq_list[node_id]++;
            pthread_mutex_unlock(&mutex);

            save_db_as_txt(); // Persist changes
        } catch (const std::exception& e) {
            std::cerr << "Error updating database: " << e.what() << "\n";
        }
    }

    /**
     * @brief Main tracker execution loop
     * 
     * Starts listener thread and runs indefinitely
     */
    void run() {
        std::string log_content = "***************** Tracker program started! *****************";
        log(0, log_content, true);

        // Start listener thread
        pthread_t thread;
        if (pthread_create(&thread, nullptr, &Tracker::listen, nullptr) != 0) {
            std::cerr << "Error: Failed to create listener thread\n";
            return;
        }

        pthread_detach(thread); // Auto-cleanup when done

        // Main thread just sleeps (could be used for admin interface)
        while (true) {
            sleep(10000);
        }
    }
};

// Initialize static class members
int Tracker::tracker_socket;
std::unordered_map<std::string, std::vector<FileOwner>> Tracker::file_owners_list;
std::unordered_map<int, int> Tracker::send_freq_list;
std::unordered_map<std::pair<int, std::pair<std::string, int>>, bool> Tracker::has_informed_tracker;
pthread_mutex_t Tracker::mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t Tracker::queue_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t Tracker::log_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t Tracker::data_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t Tracker::queue_cond = PTHREAD_COND_INITIALIZER;
std::queue<std::pair<std::unordered_map<std::string, std::any>, sockaddr_in>> Tracker::request_queue;

/**
 * @brief Main entry point
 * @return Exit status
 * 
 * Creates and runs the tracker instance
 */
int main() {
    Tracker tracker;
    tracker.run();
    return 0;
}