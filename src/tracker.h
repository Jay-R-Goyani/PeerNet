#ifndef TRACKER_H
#define TRACKER_H

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
#include <unistd.h>
#include <cerrno>
#include <cstring>

#include "utils.cpp"
#include "config.h"
#include "segment.h"
#include "messages/message.h"
#include "messages/node2tracker.h"
#include "messages/tracker2node.h"

// Custom hash functions for std::pair
struct pair_hash {
    template <typename T1, typename T2>
    std::size_t operator()(const std::pair<T1, T2>& p) const {
        std::size_t hash1 = std::hash<T1>{}(p.first);
        std::size_t hash2 = std::hash<T2>{}(p.second);
        return hash1 ^ (hash2 << 1);
    }
};

template <>
struct std::hash<std::pair<int, std::pair<std::string, int>>> {
    std::size_t operator()(const std::pair<int, std::pair<std::string, int>>& p) const {
        std::size_t h1 = std::hash<int>{}(p.first);
        std::size_t h2 = std::hash<std::string>{}(p.second.first);
        std::size_t h3 = std::hash<int>{}(p.second.second);
        return h1 ^ (h2 << 1) ^ (h3 << 2);
    }
};

class Tracker {
private:
    // Static members
    static int tracker_socket;
    static std::unordered_map<std::string, std::vector<FileOwner>> file_owners_list; // `filename` -> list of ownersList(nodeID, IP and PORT)
    static std::unordered_map<int, int> send_freq_list;                              // nodeid->frequency
    static std::unordered_map<std::pair<int, std::pair<std::string, int>>, bool> has_informed_tracker; // nodeId, (IP, PORT) -> has informed tracker(bool)
    
    // Concurrency control
    static pthread_mutex_t data_mutex;  // Protects file_owners_list, send_freq_list, has_informed_tracker
    static pthread_mutex_t log_mutex;   // Protects logging operations
    static pthread_mutex_t queue_mutex; // Protects request_queue
    static pthread_cond_t queue_cond;
    static std::queue<std::pair<std::unordered_map<std::string, std::any>, sockaddr_in>> request_queue;

public:
    Tracker() {
        tracker_socket = set_socket(Config::Constants::TRACKER_PORT);
        if (tracker_socket < 0) {
            throw std::runtime_error("Failed to create tracker socket");
        }
        pthread_mutex_init(&data_mutex, nullptr);
        pthread_mutex_init(&log_mutex, nullptr);
        pthread_mutex_init(&queue_mutex, nullptr);
        pthread_cond_init(&queue_cond, nullptr);
    }

    ~Tracker() {
        close(tracker_socket);
        pthread_mutex_destroy(&data_mutex);
        pthread_mutex_destroy(&log_mutex);
        pthread_mutex_destroy(&queue_mutex);
        pthread_cond_destroy(&queue_cond);
    }

    // Thread-safe logging wrapper
    static void log_thread_safe(int node_id, const std::string& content, bool show) {
        pthread_mutex_lock(&log_mutex);
        log(node_id, content, show);
        pthread_mutex_unlock(&log_mutex);
    }

    // Save database to text files
    static void save_db_as_txt() {
        std::string dir = Config::Directory::TRACKER_DB_DIR;
        
        // Create directory if it doesn't exist
        struct stat st;
        if (stat(dir.c_str(), &st) != 0) {
            if (mkdir(dir.c_str(), 0755) != 0) {
                log_thread_safe(0, "Error: Failed to create directory " + dir + ": " + strerror(errno), true);
                return;
            }
        } else if (!S_ISDIR(st.st_mode)) {
            log_thread_safe(0, "Error: " + dir + " exists but is not a directory", true);
            return;
        }

        std::string nodes_path = dir + "nodes_Freq_list.txt";
        std::string files_path = dir + "files_Owners_list.txt";

        // Save nodes frequency info
        std::ofstream nodes_file(nodes_path);
        if (nodes_file.is_open()) {
            pthread_mutex_lock(&data_mutex);
            for (const auto& [node_id, freq] : send_freq_list) {
                nodes_file << "node" << node_id << " " << freq << "\n";
            }
            pthread_mutex_unlock(&data_mutex);
            nodes_file.close();
        }

        // Save files ownership info
        std::ofstream files_file(files_path);
        if (files_file.is_open()) {
            pthread_mutex_lock(&data_mutex);
            for (const auto& [filename, owners] : file_owners_list) {
                files_file << filename << " : ";
                for (const auto& owner : owners) {
                    files_file << "(" << owner.node_id << ", " << owner.addr.first << ", " << owner.addr.second << ") ";
                }
                files_file << "\n";
            }
            pthread_mutex_unlock(&data_mutex);
            files_file.close();
        }
    }

    // Remove a node and all its associated data
    static void remove_node(int node_id, const std::string& ip, int port) {
        std::pair<int, std::pair<std::string, int>> entry = {node_id, {ip, port}};
    
        pthread_mutex_lock(&data_mutex);
        
        // Remove from all data structures
        send_freq_list.erase(node_id);
        has_informed_tracker.erase(entry);

        // Remove from file owners list
        for (auto it = file_owners_list.begin(); it != file_owners_list.end(); ) {
            auto& owners = it->second;
            owners.erase(
                std::remove_if(owners.begin(), owners.end(),
                    [node_id](const FileOwner& owner) { return owner.node_id == node_id; }),
                owners.end()
            );

            if (owners.empty()) {
                it = file_owners_list.erase(it);
            } else {
                ++it;
            }
        }

        pthread_mutex_unlock(&data_mutex);
        save_db_as_txt();
    }

    // Periodically check node liveness
    static void* check_nodes_periodically(void* arg) {
        int interval = Config::Constants::TRACKER_TIME_INTERVAL;
    
        while (true) {
            std::unordered_set<int> alive_nodes_ids;
            std::unordered_set<int> dead_nodes_ids;
            std::vector<std::pair<int, std::pair<std::string, int>>> to_remove;
            
            pthread_mutex_lock(&data_mutex);
            
            // Check which nodes have reported in this interval
            for (auto& [key, informed] : has_informed_tracker) {
                if (informed) {
                    informed = false; // Reset for next interval
                    alive_nodes_ids.insert(key.first);
                } else {
                    dead_nodes_ids.insert(key.first);
                    to_remove.push_back(key);
                }
            }

            pthread_mutex_unlock(&data_mutex);

            // Remove dead nodes outside the lock
            for (const auto& entry : to_remove) {
                remove_node(entry.first, entry.second.first, entry.second.second);
            }

            // Log node status
            if (!alive_nodes_ids.empty() || !dead_nodes_ids.empty()) {
                std::string log_content = "=== Node Status ===\nAlive: ";
                for (int id : alive_nodes_ids) log_content += std::to_string(id) + " ";
                log_content += "\nDead: ";
                for (int id : dead_nodes_ids) log_content += std::to_string(id) + " ";
                log_thread_safe(0, log_content, true);
            }
    
            sleep(interval);
        }
        return nullptr;
    }

    // Dispatch requests from queue to worker threads
    static void* request_dispatcher(void* arg) {
        while (true) {
            pthread_mutex_lock(&queue_mutex);
            while (request_queue.empty()) {
                pthread_cond_wait(&queue_cond, &queue_mutex);
            }
    
            auto request = request_queue.front();
            request_queue.pop();
            pthread_mutex_unlock(&queue_mutex);
    
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

    // Main listener thread
    static void* listen(void* arg) {
        // Start background threads
        pthread_t timer_thread;
        if (pthread_create(&timer_thread, nullptr, &Tracker::check_nodes_periodically, nullptr) != 0) {
            log_thread_safe(0, "Error: Failed to create timer thread", true);
            return nullptr;
        }
        pthread_detach(timer_thread);
    
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
    
                    // Add request to queue and signal dispatcher
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
    
    // Handle different types of node requests
    static void handle_node_request(const std::unordered_map<std::string, std::any>& properties, const sockaddr_in& client_addr) {
        try {
            std::string ip_addr = inet_ntoa(client_addr.sin_addr);
            int port = ntohs(client_addr.sin_port);
            std::pair<std::string, int> addr = {ip_addr, port};
    
            int mode = std::stoi(std::any_cast<std::string>(properties.at("mode")));
            int node_id = std::stoi(std::any_cast<std::string>(properties.at("node_id")));
    
            switch (mode) {
                case 1: { // OWN - Node owns a file
                    add_file_owner(properties, addr);
                    break;
                }
    
                case 2: { // NEED - Node needs a file
                    search_file(properties, addr);
                    break;
                }
    
                case 3: { // UPDATE - Update node frequency
                    update_db(properties);
                    break;
                }
    
                case 0: { // REGISTER - New node registration
                    pthread_mutex_lock(&data_mutex);
                    has_informed_tracker[{node_id, addr}] = true;
                    pthread_mutex_unlock(&data_mutex);
    
                    // Send ACK
                    std::string ack = "ACK";
                    sendto(tracker_socket, ack.c_str(), ack.size(), 0,
                        (const struct sockaddr*)&client_addr, sizeof(client_addr));
    
                    std::string log_content = "ACK sent to Node " + std::to_string(node_id) + " at " + addr.first + ":" + std::to_string(addr.second);
                    log_thread_safe(0, log_content, true);
                    break;
                }
    
                case 4: { // EXIT - Node is leaving
                    remove_node(node_id, addr.first, addr.second);
                    std::string log_content = "Node " + std::to_string(node_id) + " exited the torrent intentionally.";
                    log_thread_safe(0, log_content, true);
                    break;
                }
    
                case 5: { // HEARTBEAT - Node is alive
                    pthread_mutex_lock(&data_mutex);
                    has_informed_tracker[{node_id, addr}] = true;
                    pthread_mutex_unlock(&data_mutex);
                    break;
                }
    
                default: {
                    log_thread_safe(0, "Error: Invalid mode " + std::to_string(mode) + " received from node " + std::to_string(node_id), true);
                    break;
                }
            }
        } catch (const std::exception& e) {
            log_thread_safe(0, "Error handling node request: " + std::string(e.what()), true);
        }
    }

    // Add a file owner to the database
    static void add_file_owner(const std::unordered_map<std::string, std::any>& properties, const std::pair<std::string, int>& addr) {
        try {
            std::string filename = std::any_cast<std::string>(properties.at("filename"));
            int node_id = std::stoi(std::any_cast<std::string>(properties.at("node_id")));

            FileOwner owner = {node_id, addr};

            pthread_mutex_lock(&data_mutex);
            file_owners_list[filename].push_back(owner);
            pthread_mutex_unlock(&data_mutex);

            std::string log_content = "Node " + std::to_string(node_id) + " owns " + filename + " and is ready to send.";
            log_thread_safe(0, log_content, true);
            save_db_as_txt();
        } catch (const std::exception& e) {
            log_thread_safe(0, "Error adding file owner: " + std::string(e.what()), true);
        }
    }

    // Search for file owners
    static void search_file(const std::unordered_map<std::string, std::any>& properties, const std::pair<std::string, int>& addr) {
        try {
            std::string filename = std::any_cast<std::string>(properties.at("filename"));
            int node_id = std::stoi(std::any_cast<std::string>(properties.at("node_id")));

            std::vector<std::pair<FileOwner, int>> search_result;

            pthread_mutex_lock(&data_mutex);
            if (file_owners_list.find(filename) != file_owners_list.end()) {
                for (const auto& owner : file_owners_list[filename]) {
                    search_result.push_back({owner, send_freq_list[owner.node_id]});
                }
            }
            pthread_mutex_unlock(&data_mutex);

            std::string log_content = "Node " + std::to_string(node_id) + " is searching for " + filename;
            log_thread_safe(0, log_content, true);

            // Send response with list of owners
            Tracker2Node response(node_id, search_result, filename);
            std::vector<char> response_data = response.encode();
            send_segment(tracker_socket, response_data, addr);
        } catch (const std::exception& e) {
            log_thread_safe(0, "Error searching file: " + std::string(e.what()), true);
        }
    }

    // Send data segment to a node
    static void send_segment(int tracker_socket, const std::vector<char>& data, const std::pair<std::string, int>& addr) {
        try {
            sockaddr_in client_addr;
            memset(&client_addr, 0, sizeof(client_addr));
            client_addr.sin_family = AF_INET;
            client_addr.sin_port = htons(addr.second);
            inet_pton(AF_INET, addr.first.c_str(), &client_addr.sin_addr);

            ssize_t sent_len = sendto(tracker_socket, data.data(), data.size(), 0,
                                    (struct sockaddr*)&client_addr, sizeof(client_addr));
            if (sent_len < 0) {
                log_thread_safe(0, "sendto failed", true);
            }
        } catch (const std::exception& e) {
            log_thread_safe(0, "Error sending segment: " + std::string(e.what()), true);
        }
    }

    // Update node frequency count
    static void update_db(const std::unordered_map<std::string, std::any>& properties) {
        try {
            int node_id = std::stoi(std::any_cast<std::string>(properties.at("node_id")));

            pthread_mutex_lock(&data_mutex);
            send_freq_list[node_id]++;
            pthread_mutex_unlock(&data_mutex);

            std::string filename = std::any_cast<std::string>(properties.at("filename"));
            std::string log_content = "Node " + std::to_string(node_id) + " updated the file list for " + filename;
            log_thread_safe(0, log_content, true);
            save_db_as_txt();
        } catch (const std::exception& e) {
            log_thread_safe(0, "Error updating database: " + std::string(e.what()), true);
        }
    }

    // Main tracker run loop
    void run() {
        log_thread_safe(0, "***************** Tracker program started! *****************", true);

        pthread_t thread;
        if (pthread_create(&thread, nullptr, &Tracker::listen, nullptr) != 0) {
            log_thread_safe(0, "Error: Failed to create listener thread", true);
            return;
        }

        pthread_detach(thread);

        // Keep main thread alive
        while (true) {
            sleep(10000);
        }
    }
};

// Initialize static members
int Tracker::tracker_socket;
std::unordered_map<std::string, std::vector<FileOwner>> Tracker::file_owners_list;
std::unordered_map<int, int> Tracker::send_freq_list;
std::unordered_map<std::pair<int, std::pair<std::string, int>>, bool> Tracker::has_informed_tracker;
pthread_mutex_t Tracker::data_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t Tracker::log_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t Tracker::queue_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t Tracker::queue_cond = PTHREAD_COND_INITIALIZER;
std::queue<std::pair<std::unordered_map<std::string, std::any>, sockaddr_in>> Tracker::request_queue;

#endif // TRACKER_H