#ifndef NODE_H
#define NODE_H

// Standard Library Headers
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <pthread.h>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <ctime>
#include <chrono>
#include <cstring>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <filesystem>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <algorithm>
#include <mutex>
#include <dirent.h>
#include <cerrno>      

// Project Headers
#include "config.h" 
#include "utils.cpp"
#include "segment.h"
#include "messages/node2node.h"
#include "messages/tracker2node.h"
#include "messages/node2tracker.h"
#include "messages/chunk_sharing.h"


class Node {
    public:
    static int node_id;                                                                   // Node ID
    static int send_socket;                                                               // Socket for sending messages
    static std::unordered_set<std::string> files;                                         // Set of files owned by this node
    static std::unordered_map<std::string, std::vector<ChunkSharing>> downloaded_files;   // Map of downloaded files and their chunks

    static pthread_mutex_t mutex_lock; // Mutex for thread safety
    static pthread_mutex_t log_mutex; // Mutex for thread safety

    Node() {
        pthread_mutex_init(&mutex_lock, nullptr);
        pthread_mutex_init(&log_mutex, nullptr);
    }

    ~Node() {
        free_socket(send_socket); // Free the socket
        pthread_mutex_destroy(&mutex_lock);
        pthread_mutex_destroy(&log_mutex);
    }

    // Initialize the node with the given ID, receive port, and send port
    static void init(int id, int rcv_port, int send_port) {
        node_id = id; 
        send_socket = set_socket(send_port); 

        // Create base directories
        if (!create_directory_recursive(Config::Directory::LOGS_DIR)) {
            log_thread_safe(node_id, "Warning: Failed to create logs directory");
        }
        if (!create_directory_recursive(Config::Directory::NODE_FILES_DIR)) {
            log_thread_safe(node_id, "Warning: Failed to create node_files directory");
        }
        if (!create_directory_recursive(Config::Directory::TRACKER_DB_DIR)) {
            log_thread_safe(node_id, "Warning: Failed to create tracker_db directory");
        }

        files = fetch_owned_files(); // Fetch the list of files owned by this node
    }

    static void log_thread_safe(int node_id, const std::string& content) {
        pthread_mutex_lock(&log_mutex);
        log(node_id, content);
        pthread_mutex_unlock(&log_mutex);
    }
    
    // Fetch the list of files owned by this node
    static std::unordered_set<std::string> fetch_owned_files() {
        std::unordered_set<std::string> files; // Set to store file names
        std::string node_files_dir = std::string(Config::Directory::NODE_FILES_DIR) + "node" + std::to_string(node_id);

        if (!create_directory_recursive(node_files_dir)) {
            log_thread_safe(node_id, "Warning: Could not create node files directory");
            return files;
        }
    
        struct stat st;
        if (stat(node_files_dir.c_str(), &st) != 0) {
            if (mkdir(node_files_dir.c_str(), 0755) != 0) {
                std::cerr << "Error creating directory " << node_files_dir 
                          << ": " << strerror(errno) << std::endl;
                return files;
            }
            return files;
        } else if (!S_ISDIR(st.st_mode)) {
            std::cerr << "Error: " << node_files_dir << " exists but is not a directory" << std::endl;
            return files;
        }
    
        // Open the directory for reading
        DIR* dir = opendir(node_files_dir.c_str());
        if (!dir) {
            std::cerr << "Error opening directory " << node_files_dir 
                      << ": " << strerror(errno) << std::endl;
            return files;
        }
    
        // Read directory entries
        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            // Skip the special entries "." and ".."
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                continue;
            }
    
            std::string full_path = node_files_dir + "/" + entry->d_name;
            
            // Check if the entry is a regular file
            if (stat(full_path.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
                files.insert(entry->d_name); 
            }
        }
    
        closedir(dir); 
        return files; 
    }

    static bool create_directory_recursive(const std::string& path) {
        static std::mutex dir_mutex;
        std::lock_guard<std::mutex> lock(dir_mutex);
        
        size_t pos = 0;
        std::string dir;
        int mdret;
        
        if (path[path.size()-1] != '/') {
            dir = path + "/";
        }
        
        while ((pos = dir.find_first_of('/', pos)) != std::string::npos) {
            std::string subdir = dir.substr(0, pos++);
            if (subdir.empty()) continue; // Skip leading /
            
            struct stat st;
            if (stat(subdir.c_str(), &st) == -1) {
                mdret = mkdir(subdir.c_str(), 0755);
                if (mdret != 0 && errno != EEXIST) {
                    log_thread_safe(node_id, "Error creating directory " + subdir + ": " + strerror(errno));
                    return false;
                }
            } else if (!S_ISDIR(st.st_mode)) {
                log_thread_safe(node_id, "Error: " + subdir + " exists but is not a directory");
                return false;
            }
        }
        return true;
    }



    // Network Communication Functions
    // Function to send a data segment to a specified address using a UDP socket
    static bool send_segment(int send_socket, const std::vector<char>& data, const std::pair<std::string, int>& addr) {
        if (send_socket < 0) {
            log_thread_safe(node_id, "Error: Invalid socket in send_segment.");
            return false; 
        }
    
        if (data.empty()) {
            log_thread_safe(node_id, "Error: Empty data vector in send_segment.");
            return false; 
        }
    
        struct sockaddr_in sock_addr;
        socklen_t addr_len = sizeof(sock_addr);
        if (getsockname(send_socket, (struct sockaddr*)&sock_addr, &addr_len) == -1) {
            perror("getsockname failed"); 
            log_thread_safe(node_id, "Error: Failed to get socket name.");
            return false; 
        }
    
        UDPSegment segment(ntohs(sock_addr.sin_port), addr.second, data);
    
        // Prepare the destination address structure
        sockaddr_in client_addr;
        memset(&client_addr, 0, sizeof(client_addr)); 
        client_addr.sin_family = AF_INET;
        client_addr.sin_port = htons(addr.second); // Set the destination port
        client_addr.sin_addr.s_addr = inet_addr(addr.first.c_str()); // Set the destination IP address
    
        ssize_t sent_len = sendto(send_socket, data.data(), data.size(), 0,
                                  (struct sockaddr*)&client_addr, sizeof(client_addr));
        if (sent_len < 0) {
            perror("sendto failed"); 
            log_thread_safe(node_id, "Error: Failed to send data to " + addr.first + ":" + std::to_string(addr.second));
            return false; 
        }
    
        return true; 
    }


    // Tracker Communication Functions
    // Function to register the node in the torrent system by informing the tracker
    void enter_torrent() {

        Node2Tracker msg(node_id, Config::TrackerRequestsMode::REGISTER, "");

        // Send the registration message to the tracker
        send_segment(send_socket, msg.encode(), {Config::Constants::TRACKER_IP, Config::Constants::TRACKER_PORT});

        // Set a timeout for receiving the acknowledgment (ACK) from the tracker
        struct timeval timeout;
        timeout.tv_sec = 2;  // Timeout duration: 2 seconds
        timeout.tv_usec = 0;
        setsockopt(send_socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));


        char buffer[Config::Constants::BUFFER_SIZE];
        sockaddr_in from_addr; 
        socklen_t from_len = sizeof(from_addr);

        ssize_t recv_len = recvfrom(send_socket, buffer, sizeof(buffer), 0,
                                    (struct sockaddr*)&from_addr, &from_len);

        // Check if an ACK was received
        if (recv_len > 0) {

            std::string ack(buffer, buffer + recv_len);

            if (ack == "ACK") {
                log_thread_safe(node_id, "ACK received from Tracker"); 
            } else {
                log_thread_safe(node_id, "Unexpected message instead of ACK: " + ack); 
            }
        } else {
            log_thread_safe(node_id, "ACK not received within timeout. Tracker might be down.");
            return;
        }

        log_thread_safe(node_id, "Entered Torrent.");
    }

    // Function to gracefully exit the torrent system
    void exit_torrent() {
        // Create a message to notify the tracker that this node is exiting
        Node2Tracker msg(node_id, Config::TrackerRequestsMode::EXIT, "");

        send_segment(send_socket, msg.encode(), {Config::Constants::TRACKER_IP, Config::Constants::TRACKER_PORT});

        log_thread_safe(node_id, "You exited the torrent!");
    }

    // Thread function to periodically inform the tracker that the node is still active
    static void* inform_tracker_periodically(void* arg) {
        // Interval between heartbeat messages (in seconds)
        int interval = Config::Constants::NODE_TIME_INTERVAL;

        while (true) {
            std::string log_contain = "I informed the tracker that I'm still alive in the torrent!";
            log_thread_safe(node_id, log_contain);

            // Create a heartbeat message to notify the tracker
            Node2Tracker msg(node_id, Config::TrackerRequestsMode::HEARTBEAT, "");

            send_segment(send_socket, msg.encode(), {Config::Constants::TRACKER_IP, Config::Constants::TRACKER_PORT});

            // Sleep for the specified interval before sending the next heartbeat
            sleep(interval);
        }

        return nullptr; 
    }

    // Searches the torrent system for nodes that have the specified file
    static std::vector<std::pair<FileOwner, int>> search_torrent(std::string &filename) {
        // Create a message to request the file from the tracker
        Node2Tracker msg(node_id, Config::TrackerRequestsMode::NEED, filename);

        // Generate a temporary port and socket for communication
        int temp_port = generate_random_port();
        int temp_sock = set_socket(temp_port);

        // Send the request message to the tracker
        send_segment(temp_sock, msg.encode(), {Config::Constants::TRACKER_IP, Config::Constants::TRACKER_PORT});
        
        while (true) {
            char buffer[Config::Constants::BUFFER_SIZE]; 
            sockaddr_in addr; 
            socklen_t addr_len = sizeof(addr); 

            // Receive data from the tracker
            int bytes_received = recvfrom(temp_sock, buffer, sizeof(buffer), 0, (struct sockaddr*)&addr, &addr_len);
            if (bytes_received > 0) {
                // Decode the received data into a Tracker2Node object
                Tracker2Node result = Tracker2Node::decode(std::vector<char>(buffer, buffer + bytes_received));

                free_socket(temp_sock); // Free the temporary socket
                return result.search_result;
            }
        }
    }
        


    // File Sharing Functions
    // Sends the size of the requested file to the requesting node
    static void send_file_size(Node2Node &result, const sockaddr_in& addr) {
        std::string filename = result.filename;

        std::string file_path = std::string(Config::Directory::NODE_FILES_DIR) + 
                              "node" + std::to_string(node_id) + "/" + filename;
    
        // Check if the file exists and retrieve its metadata using stat
        struct stat file_stat;
        if (stat(file_path.c_str(), &file_stat) != 0) {
            log_thread_safe(node_id, "File not found: " + filename + (errno != ENOENT ? " (" + std::string(strerror(errno)) + ")" : ""));
            return;
        }
    
        // Verify that the path corresponds to a regular file
        if (!S_ISREG(file_stat.st_mode)) {
            log_thread_safe(node_id, "Path is not a regular file: " + filename);
            return;
        }
    
        int size = file_stat.st_size;
        int dest_node_id = result.src_node_id; 
    
        // Create a Node2Node message containing the file size
        Node2Node msg(node_id, dest_node_id, filename, size);

        std::string ip_str = inet_ntoa(addr.sin_addr);
        int port = ntohs(addr.sin_port);

        // Send the file size to the requesting node
        send_segment(send_socket, msg.encode(), {ip_str, port});
    }

    // Splits a file into chunks based on the specified range
    static std::vector<std::vector<char>> split_file_to_chunks(const std::string& file_path, std::pair<int, int> rng) {
        std::vector<std::vector<char>> chunks; 

        // Open the file in binary mode
        std::ifstream file(file_path, std::ios::binary);
        if (!file) {
            std::cerr << "Error opening file: " << std::strerror(errno) << std::endl;
            return chunks;
        }
    
        // Validate the specified range
        if (rng.first < 0 || rng.second < rng.first) {
            std::cerr << "Invalid range specified" << std::endl;
            return chunks;
        }
    
        // Seek to the start position of the range
        file.seekg(rng.first, std::ios::beg);
        if (!file) {
            std::cerr << "Error seeking in file: " << std::strerror(errno) << std::endl;
            return chunks;
        }
    
        size_t chunk_size = rng.second - rng.first;
        std::vector<char> buffer(chunk_size); 
    
        file.read(buffer.data(), chunk_size);
        size_t bytes_read = file.gcount(); 
        if (bytes_read != chunk_size) {
            std::cerr << "Error reading file: only " << bytes_read << " bytes read out of " << chunk_size << std::endl;
            return chunks;
        }
    
        size_t piece_size = Config::Constants::CHUNK_PIECES_SIZE;
        if (piece_size == 0) {
            std::cerr << "Invalid piece size: must be greater than 0" << std::endl;
            return chunks;
        }
    
        // Iterate through the buffer and create smaller pieces
        for (size_t p = 0; p < buffer.size(); p += piece_size) {
            size_t end = std::min(p + piece_size, buffer.size());
            chunks.emplace_back(buffer.begin() + p, buffer.begin() + end); 
        }
    
        return chunks; 
    }

    // Sends a chunk of a file to a destination node
    static void send_chunk(const std::string& filename, std::pair<int, int> range, int dest_node_id, int dest_port, const std::string& dest_ip) {
        if (range.first < 0 || range.second < 0 || range.first > range.second) {
            log_thread_safe(node_id, "Error: Invalid range specified for file " + filename);
            return;
        }
    
        auto start_time = std::chrono::high_resolution_clock::now(); // Start timing
        
        std::string file_path = std::string(Config::Directory::NODE_FILES_DIR) + 
                              "node" + std::to_string(node_id) + "/" + 
                              filename;
    
        struct stat file_stat;
        if (stat(file_path.c_str(), &file_stat) != 0 || !S_ISREG(file_stat.st_mode)) {
            log_thread_safe(node_id, "Error: File not found or inaccessible: " + filename + 
                (errno != ENOENT ? " (" + std::string(strerror(errno)) + ")" : ""));
            return;
        }
    
        // Get file size for logging
        off_t file_size = file_stat.st_size;
        log_thread_safe(node_id, "Starting to send file: " + filename + " (Size: " + std::to_string(file_size) + " bytes)");
    
        // Split the file into chunks based on the specified range
        std::vector<std::vector<char>> chunk_pieces = split_file_to_chunks(file_path, range);
        if (chunk_pieces.empty()) {
            log_thread_safe(node_id, "Error: Failed to split file into chunks for " + filename);
            return;
        }
    
        static std::mutex socket_mutex;
        int temp_port, temp_sock;
        {
            std::lock_guard<std::mutex> lock(socket_mutex);
            temp_port = generate_random_port(); 
            temp_sock = set_socket(temp_port); 
        }
        if (temp_sock < 0) {
            log_thread_safe(node_id, "Error: Failed to create temporary socket.");
            return;
        }
    
        size_t total_bytes_sent = 0;
        
        // Send each chunk to the destination node
        for (size_t idx = 0; idx < chunk_pieces.size(); ++idx) {
            if (chunk_pieces[idx].size() > Config::Constants::CHUNK_PIECES_SIZE) {
                log_thread_safe(node_id, "Error: Chunk " + std::to_string(idx) + " exceeds maximum size for file " + filename);
                return;
            }
    
            // Create a ChunkSharing message for the current chunk
            ChunkSharing msg(node_id, dest_node_id, filename, range, idx, chunk_pieces[idx]);
    
            // Send the chunk to the destination node
            if (!send_segment(temp_sock, msg.encode(), {dest_ip, dest_port})) {
                log_thread_safe(node_id, "Error: Failed to send chunk " + std::to_string(idx) + " for file " + filename);
                return;
            }
    
            total_bytes_sent += chunk_pieces[idx].size();
            // log_thread_safe(node_id, "Sent chunk " + std::to_string(idx) + "/" + std::to_string(chunk_pieces.size()) + " (" + std::to_string(chunk_pieces[idx].size()) + " bytes) for file " + filename);
        }
    
        // Send a termination signal to indicate the end of transmission
        ChunkSharing msg(node_id, dest_node_id, filename, range, -1, {});
        if (!send_segment(temp_sock, msg.encode(), {dest_ip, dest_port})) {
            log_thread_safe(node_id, "Error: Failed to send termination signal for file " + filename);
            return;
        }
    
        auto end_time = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
        
        log_thread_safe(node_id, "Finished sending chunks for file: " + filename + " to Node " + std::to_string(dest_node_id));
        log_thread_safe(node_id, "Total bytes sent: " + std::to_string(total_bytes_sent) + 
            " in " + std::to_string(duration.count()) + " ms");
        log_thread_safe(node_id, "Average speed: " + 
            std::to_string((total_bytes_sent * 1000) / (duration.count() * 1024)) + " KB/s");
    
        // Notify the tracker about the file update
        Node2Tracker to_tracker_msg(node_id, Config::TrackerRequestsMode::UPDATE, filename);
        if (!send_segment(temp_sock, to_tracker_msg.encode(), {Config::Constants::TRACKER_IP, Config::Constants::TRACKER_PORT})) {
            log_thread_safe(node_id, "Error: Failed to notify tracker about file " + filename);
        }

        free_socket(temp_sock); // Free the temporary socket
    }

    // Thread function to handle ping requests (run in listen thread)
    static void handle_ping_request(const sockaddr_in& addr) {
        const std::string pong_msg = "PONG";
        std::string ip_str = inet_ntoa(addr.sin_addr);
        int port = ntohs(addr.sin_port);    
        log_thread_safe(node_id, "Received PING from " + ip_str + ":" + std::to_string(port));
        send_segment(send_socket, std::vector<char>(pong_msg.begin(), pong_msg.end()), {ip_str, port});  
        log_thread_safe(node_id, "Sending PONG to " + ip_str + ":" + std::to_string(port));
    }

    // Handles incoming requests from other nodes
    static void handle_requests(char buffer[], int bytes_received, const sockaddr_in& addr) {

        std::string msg(buffer, bytes_received);
        
        // Handle ping-pong messages first
        if (msg == "PING") {
            handle_ping_request(addr);
            return;
        } 

        std::unordered_map<std::string, std::any> properties = Message::decode(std::vector<char>(buffer, buffer + bytes_received));
        
        std::string filename = std::any_cast<std::string>(properties.at("filename"));

        // Check if the message contains a "size" property
        if (properties.find("size") != properties.end()) {
            Node2Node result = Node2Node::decode(std::vector<char>(buffer, buffer + bytes_received));

            int size = result.size;
            if (size == -1) {
                // If size is -1, it indicates a request for the file size
                log_thread_safe(node_id, "Received a request for size of " + filename);
                send_file_size(result, addr); // Respond with the file size
            }
        } 
        // Check if the message contains a "range_start" property
        else if (properties.find("range_start") != properties.end()) {
            ChunkSharing result = ChunkSharing::decode(std::vector<char>(buffer, buffer + bytes_received));

            std::pair<int, int> range = result.range;
            int dest_node_id = result.src_node_id;

            char ipStr[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &(addr.sin_addr), ipStr, INET_ADDRSTRLEN);
            std::string dest_IP_addr(ipStr);  

            int dest_port = ntohs(addr.sin_port);

            std::vector<char> chunk = result.chunk;

            // If the chunk is empty, it indicates a request for the chunk
            if (chunk.empty()) {
                // send the requested chunk to the destination node
                send_chunk(filename, range, dest_node_id, dest_port, dest_IP_addr);
            }
        }
    }

    // Thread function to listen for incoming requests from other nodes
    static void* listen(void* arg) {
        while (true) {
            char buffer[Config::Constants::BUFFER_SIZE];
            sockaddr_in addr; 
            socklen_t addr_len = sizeof(addr);

            // Receive data from the socket
            int bytes_received = recvfrom(send_socket, buffer, sizeof(buffer), 0, (struct sockaddr*)&addr, &addr_len);

            if (bytes_received > 0) {
                pthread_t request_thread;
                auto* args = new std::tuple<char*, int, sockaddr_in>(buffer, bytes_received, addr);
                pthread_create(&request_thread, nullptr, [](void* arg) -> void* {
                    auto* params = static_cast<std::tuple<char*, int, sockaddr_in>*>(arg);
                    char* buffer = std::get<0>(*params);
                    int bytes_received = std::get<1>(*params);
                    sockaddr_in addr = std::get<2>(*params);
                    handle_requests(buffer, bytes_received, addr);
                    delete params;
                    return nullptr;
                }, args);
                pthread_detach(request_thread);
            }
        }
        return nullptr; 
    }

    // Sets the node to send mode for the specified file
    static void set_send_mode(const std::string& filename) {
        pthread_mutex_lock(&mutex_lock);
        if (files.find(filename) == files.end()) {
            log_thread_safe(node_id, "You don't have " + filename); 
            return;
        }
        pthread_mutex_unlock(&mutex_lock);

        // Notify the tracker that this node owns the file
        Node2Tracker msg(node_id, Config::TrackerRequestsMode::OWN, filename);
        send_segment(send_socket, msg.encode(), {Config::Constants::TRACKER_IP, Config::Constants::TRACKER_PORT});  

        log_thread_safe(node_id, "FILE ENTRY REGISTERED! You are waiting for other nodes' requests!"); 

        // Start a listener thread to handle incoming requests
        pthread_t listener_thread;
        pthread_create(&listener_thread, nullptr, listen, nullptr);
        pthread_detach(listener_thread); 

    }



    // File Download Functions
    // function to initiate download mode for a specific file
    static void* set_download_mode(void* arg) {
        std::string filename = *((std::string*)arg);

        std::string file_path = std::string(Config::Directory::NODE_FILES_DIR) + "node" + std::to_string(node_id) + "/" + filename;

        struct stat buffer;
        if (stat(file_path.c_str(), &buffer) == 0) {
            log_thread_safe(node_id, "You already have this file!");
            return nullptr;
        } else {
            log_thread_safe(node_id, "Let's search " + filename + " in the torrent!");

            std::vector<std::pair<FileOwner, int>> file_owners = search_torrent(filename);

            if (file_owners.empty()) {
                log_thread_safe(node_id, "No one has " + filename);
                return nullptr;
            }

            // Split the file among the available file owners and download it
            split_file_owners(file_owners, filename);
        }
        return nullptr;
    }


    // Function to request and retrieve the size of a file from a specific file owner
    static int ask_file_size(const std::string& filename, const FileOwner& owner) {
        int temp_port = generate_random_port();
        int temp_sock = set_socket(temp_port);
    
        // Create a Node2Node message to request the file size (size = -1 indicates a request)
        Node2Node msg(node_id, owner.node_id, filename, -1);
        send_segment(temp_sock, msg.encode(), {owner.addr.first, owner.addr.second});
    
        // Wait for a response from the file owner
        while (true) {
            char buffer[Config::Constants::BUFFER_SIZE]; 
            sockaddr_in addr; 
            socklen_t addr_len = sizeof(addr); 

            // Receive data from the socket
            int bytes_received = recvfrom(temp_sock, buffer, sizeof(buffer), 0, (sockaddr*)&addr, &addr_len);
            if (bytes_received > 0) {
                // Decode the received data into a Node2Node object
                Node2Node result = Node2Node::decode(std::vector<char>(buffer, buffer + bytes_received));
                int size = result.size;

                // If a valid size is received, close the socket and return the size
                if (size > 0) {
                    free_socket(temp_sock);
                    return size;
                }
            }
        }
    }

    // Measures latency to a peer using UDP ping-pong
    static long measure_latency(const std::string& ip, int port, int rec_node_id) {
        int temp_port = generate_random_port();
        int temp_sock = set_socket(temp_port);
        if (temp_sock < 0) {
            log_thread_safe(node_id, "Error creating socket for latency measurement");
            return -1;
        }

        // Set timeout for ping
        struct timeval timeout;
        timeout.tv_sec = 5;  // 5 second timeout
        timeout.tv_usec = 0;
        setsockopt(temp_sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

        const std::string ping_msg = "PING";
        auto start = std::chrono::high_resolution_clock::now();

        log_thread_safe(node_id, "Sending PING to " + ip + ":" + std::to_string(port) + " node_id: " + std::to_string(rec_node_id));
        // Send ping
        if (!send_segment(temp_sock, std::vector<char>(ping_msg.begin(), ping_msg.end()), {ip, port})) {
            free_socket(temp_sock);
            return -1;
        }

        // Wait for pong
        char buffer[Config::Constants::BUFFER_SIZE];
        sockaddr_in from_addr;
        socklen_t from_len = sizeof(from_addr);
        ssize_t recv_len = recvfrom(temp_sock, buffer, sizeof(buffer), 0,
                                   (struct sockaddr*)&from_addr, &from_len);

        log_thread_safe(node_id, "Received PONG from " + ip + ":" + std::to_string(port) + " node_id: " + std::to_string(rec_node_id));

        free_socket(temp_sock);

        if (recv_len <= 0) {            
            return -1;  // Timeout or error
        }

        auto end = std::chrono::high_resolution_clock::now();
        long latency = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();     
        log_thread_safe(node_id, "Latency to " + ip + ":" + std::to_string(port) + " is " + std::to_string(latency) + "ms");
        return latency;
    }

    // Selects best K peers based on latency
    static std::vector<FileOwner> select_best_peers(const std::vector<std::pair<FileOwner, int>>& peers, int k) {
        std::vector<std::pair<FileOwner, long>> peers_with_latency;
        
        // Measure latency to each peer
        for (const auto& peer : peers) {
            long latency = measure_latency(peer.first.addr.first, peer.first.addr.second, peer.first.node_id);
            if (latency >= 0) {  // Only consider responsive peers
                peers_with_latency.emplace_back(peer.first, latency);
                log_thread_safe(node_id, "Latency to node " + std::to_string(peer.first.node_id) + 
                    ": " + std::to_string(latency) + "ms");
            }
        }
        
        // Sort by latency (ascending)
        std::sort(peers_with_latency.begin(), peers_with_latency.end(),
            [](const auto& a, const auto& b) {
                return a.second < b.second;
            });
        
        // Select top K peers
        std::vector<FileOwner> best_peers;
        for (int i = 0; i < std::min(k, (int)peers_with_latency.size()); i++) {
            best_peers.push_back(peers_with_latency[i].first);
        }
        
        return best_peers;
    }

    // Function to split the file among available file owners and download it
    static void split_file_owners(std::vector<std::pair<FileOwner, int>>& file_owners, const std::string& filename) {
        auto download_start_time = std::chrono::high_resolution_clock::now();
        
        // Filter out the current node from the list of file owners
        std::vector<std::pair<FileOwner, int>> owners;
        for (const auto& owner : file_owners) {
            if (owner.first.node_id != node_id) {
                owners.push_back(owner);
            }
        }
    
        if (owners.empty()) {
            log_thread_safe(node_id, "No one has " + filename);
            return;
        }
        
        // Select best peers based on latency instead of send frequency
        std::vector<FileOwner> to_be_used_owners = select_best_peers(owners, Config::Constants::MAX_SPLITTNES_RATE);
    
        if (to_be_used_owners.empty()) {
            log_thread_safe(node_id, "No responsive peers found for " + filename);
            return;
        }
    
        std::string log_content = "Downloading " + filename + " from nodes: ";
        for (const auto& owner : to_be_used_owners) {
            log_content += std::to_string(owner.node_id) + " "; 
        }
        log_thread_safe(node_id, log_content);
    
        // Request the file size from the first peer (to determine the file size for splitting)
        int file_size = ask_file_size(filename, to_be_used_owners[0]);
        log_thread_safe(node_id, "File " + filename + " size: " + std::to_string(file_size) + " bytes");
    
        // Split the file equally among the selected peers
        int step = file_size / to_be_used_owners.size(); 
        int remainder = file_size % to_be_used_owners.size();  
        std::vector<std::pair<int, int>> chunks_ranges; 
        for (int i = 0; i < to_be_used_owners.size(); i++) {
            int start = step * i;
            int end = (i == to_be_used_owners.size() - 1) ? start + step + remainder : start + step;
            chunks_ranges.emplace_back(start, end);  
        }
    
        // Lock mutex to ensure thread-safety when modifying the downloaded_files map
        static std::mutex downloaded_files_mutex;
        {
            pthread_mutex_lock(&mutex_lock);
            downloaded_files[filename] = {};
            pthread_mutex_unlock(&mutex_lock);  
        }
    
        // Create threads to download chunks concurrently
        std::vector<pthread_t> threads(to_be_used_owners.size());
        for (size_t i = 0; i < to_be_used_owners.size(); i++) {
            // Prepare arguments for each thread (filename, chunk range, and owner)
            auto args = new std::tuple<std::string, std::pair<int, int>, FileOwner>(filename, chunks_ranges[i], to_be_used_owners[i]);
            
            // Create a new thread to download the chunk
            if (pthread_create(&threads[i], nullptr, receive_chunk, args) != 0) {
                log_thread_safe(node_id, "Error: Failed to create thread for chunk " + std::to_string(i));
                delete args; 
            }
        }
    
        // Wait for all threads to finish downloading their respective chunks
        for (auto& thread : threads) {
            pthread_join(thread, nullptr);
        }
    
        auto download_end_time = std::chrono::high_resolution_clock::now();
        auto download_duration = std::chrono::duration_cast<std::chrono::milliseconds>(download_end_time - download_start_time);
        
        log_thread_safe(node_id, "All chunks of " + filename + " downloaded in " + 
            std::to_string(download_duration.count()) + " ms");
        log_thread_safe(node_id, "Average download speed: " + 
            std::to_string((file_size * 1000) / (download_duration.count() * 1024)) + " KB/s");
    
        log_thread_safe(node_id, "Sorting chunks now...");
    
        // Sort the downloaded chunks based on their ranges
        std::vector<ChunkSharing> sorted_chunks = sort_downloaded_chunks(filename);
        log_thread_safe(node_id, "All chunks sorted. Reassembling file...");
    
        std::string file_path = std::string(Config::Directory::NODE_FILES_DIR) + 
                                "node" + std::to_string(node_id) + "/" + filename;
    
        std::string dir_path = std::string(Config::Directory::NODE_FILES_DIR) + 
                               "node" + std::to_string(node_id);
        struct stat st;
        if (stat(dir_path.c_str(), &st) != 0) {
            if (mkdir(dir_path.c_str(), 0755) != 0) {
                log_thread_safe(node_id, "Error creating directory: " + dir_path + " (" + strerror(errno) + ")");
                return; 
            }
        }
    
        auto reassemble_start_time = std::chrono::high_resolution_clock::now();
        
        // Reassemble the file using the sorted chunks and save it
        reassemble_file(sorted_chunks, file_path);
        
        auto reassemble_end_time = std::chrono::high_resolution_clock::now();
        auto reassemble_duration = std::chrono::duration_cast<std::chrono::milliseconds>(reassemble_end_time - reassemble_start_time);
        
        log_thread_safe(node_id, filename + " successfully reassembled in " + 
            std::to_string(reassemble_duration.count()) + " ms");
        log_thread_safe(node_id, "Total download and reassembly time: " + 
            std::to_string((download_duration + reassemble_duration).count()) + " ms");
    

        pthread_mutex_lock(&mutex_lock);
        files.insert(filename); 
        pthread_mutex_unlock(&mutex_lock);
    
        // Inform the tracker that this node now has the file
        set_send_mode(filename);
    }
    
    // Reassemble the full file from its received chunks and write to disk
    static void reassemble_file(std::vector<ChunkSharing>& chunks, const std::string& file_path) {
        if (chunks.empty()) {
            log_thread_safe(node_id, "Error: No chunks provided for reassembly");
            return;
        }

        std::string dir_path = std::string(Config::Directory::NODE_FILES_DIR) + 
                       "node" + std::to_string(node_id);
        if (!create_directory_recursive(dir_path)) {
            log_thread_safe(node_id, "Error creating directory: " + dir_path);
            return; 
        }

        // Open the output file in binary write mode, truncating any existing content
        std::ofstream file(file_path, std::ios::binary | std::ios::out | std::ios::trunc);
        if (!file) {
            log_thread_safe(node_id, "Failed to open file: " + file_path + " while assembling");
            return;
        }

        for (const auto& chunk : chunks) {
            if (!file.write(chunk.chunk.data(), chunk.chunk.size())) {
                log_thread_safe(node_id, "Error: Failed to write chunk to file " + file_path);
                return;
            }
        }

        chunks.clear(); 

        // Ensure all data is flushed to disk
        if (!file.flush()) {
            log_thread_safe(node_id, "Error: Failed to flush file " + file_path);
            return;
        }

        file.close();

        log_thread_safe(node_id, "File successfully reassembled: " + file_path);
    }

    // Thread function to request and receive a file chunk from another node
    static void* receive_chunk(void* arg) {
        // Extract arguments: filename, chunk range, and file owner's info
        auto* args = static_cast<std::tuple<std::string, std::pair<int, int>, FileOwner>*>(arg);
        std::string filename = std::get<0>(*args);
        std::pair<int, int> range = std::get<1>(*args);
        FileOwner file_owner = std::get<2>(*args);
        delete args;

        int dest_node_id = file_owner.node_id;

        // Set up a temporary socket for receiving the chunk
        int temp_port = generate_random_port();
        int temp_sock = set_socket(temp_port);
        if (temp_sock < 0) {
            log_thread_safe(node_id, "Error: Failed to create temporary socket");
            return nullptr;
        }

        // Send a request message for the chunk (idx = -1 indicates request)
        ChunkSharing msg(node_id, dest_node_id, filename, range, -1);
        if (!send_segment(temp_sock, msg.encode(), {file_owner.addr.first, file_owner.addr.second})) {
            log_thread_safe(node_id, "Error: Failed to send request for chunk of " + filename + " to node " + std::to_string(dest_node_id));
            free_socket(temp_sock);
            return nullptr;
        }

        log_thread_safe(node_id, "I sent a request for a chunk of " + filename + " for node " + std::to_string(dest_node_id));

        int retry_count = 0;
        const int MAX_RETRIES = 10;

        // Try receiving the chunk with retry mechanism
        while (retry_count < MAX_RETRIES) {
            std::vector<char> buffer(Config::Constants::BUFFER_SIZE);
            sockaddr_in sender_addr;
            socklen_t sender_len = sizeof(sender_addr);

            ssize_t bytes_received = recvfrom(temp_sock, buffer.data(), buffer.size(), 0, (sockaddr*)&sender_addr, &sender_len);
            if (bytes_received <= 0) {
                retry_count++;
                continue;
            }

            // Decode the received data into a ChunkSharing object
            ChunkSharing result = ChunkSharing::decode(std::vector<char>(buffer.begin(), buffer.begin() + bytes_received));
            int idx = result.idx;

            // If idx = -1, it indicates the end of transmission
            if (idx == -1) {
                free_socket(temp_sock);
                return nullptr;
            }

            // Store the received chunk in a thread-safe manner
            pthread_mutex_lock(&mutex_lock);
            downloaded_files[filename].push_back(result);
            pthread_mutex_unlock(&mutex_lock);

            retry_count = 0; 
        }

        // If retries exceeded, log an error and exit
        log_thread_safe(node_id, "Error: Maximum retries reached for receiving chunks of " + filename);
        free_socket(temp_sock);
        return nullptr;
    }

    // Sorts and returns the downloaded chunks of a file based on range and chunk index
    static std::vector<ChunkSharing> sort_downloaded_chunks(const std::string& filename) {

        pthread_mutex_lock(&mutex_lock);
        // Check if the file has any downloaded chunks
        if (downloaded_files.find(filename) == downloaded_files.end()) {
            log_thread_safe(node_id, "No downloaded chunks found for " + filename);
            return {};
        }

        auto& chunks = downloaded_files[filename];
        pthread_mutex_unlock(&mutex_lock);

        // Sort chunks by start of range, then by chunk index
        std::sort(chunks.begin(), chunks.end(),
            [](const ChunkSharing& a, const ChunkSharing& b) {
                if (a.range.first != b.range.first)
                    return a.range.first < b.range.first;
                return a.idx < b.idx;
            });

        return chunks;
    }

    // Search for file owners
    void search_file_owners(std::string& filename) {
        std::vector<std::pair<FileOwner, int>> file_owners = search_torrent(filename);
        
        if (file_owners.empty()) {
            log_thread_safe(node_id, "No owners found for file: " + filename);
        } else {
            log_thread_safe(node_id, "Owners of file " + filename + ":");
            for (const auto& owner : file_owners) {
                log_thread_safe(node_id, "Node " + std::to_string(owner.first.node_id) + 
                    " (" + owner.first.addr.first + ":" + std::to_string(owner.first.addr.second) + ")");
            }
        }
    }

};
pthread_mutex_t Node::mutex_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t Node::log_mutex = PTHREAD_MUTEX_INITIALIZER;
#endif