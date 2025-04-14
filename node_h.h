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


static std::mutex download_mutex; // Mutex for synchronizing access to downloaded_files

class Node {
    public:
    static int node_id;                                                                   // Node ID
    static int send_socket;                                                               // Socket for sending messages
    static std::unordered_set<std::string> files;                                         // Set of files owned by this node
    static std::unordered_map<std::string, std::vector<ChunkSharing>> downloaded_files;   // Map of downloaded files and their chunks

    // Initialize the node with the given ID, receive port, and send port
    static void init(int id, int rcv_port, int send_port) {
        node_id = id; // Set the node ID
        send_socket = set_socket(send_port); // Create and bind the send socket
        files = fetch_owned_files(); // Fetch the list of files owned by this node
    }
    
    // Fetch the list of files owned by this node
    static std::unordered_set<std::string> fetch_owned_files() {
        std::unordered_set<std::string> files; // Set to store file names
        std::string node_files_dir = std::string(Config::Directory::NODE_FILES_DIR) + "node" + std::to_string(node_id);
    
        // Check if the directory exists using stat
        struct stat st;
        if (stat(node_files_dir.c_str(), &st) != 0) {
            // If the directory doesn't exist, create it
            if (mkdir(node_files_dir.c_str(), 0755) != 0) {
                std::cerr << "Error creating directory " << node_files_dir 
                          << ": " << strerror(errno) << std::endl;
                return files; // Return empty set if directory creation fails
            }
            return files; // Return empty set for a newly created directory
        } else if (!S_ISDIR(st.st_mode)) {
            // If the path exists but is not a directory, log an error
            std::cerr << "Error: " << node_files_dir << " exists but is not a directory" << std::endl;
            return files;
        }
    
        // Open the directory for reading
        DIR* dir = opendir(node_files_dir.c_str());
        if (!dir) {
            // Log an error if the directory cannot be opened
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
    
            // Build the full path to check the file type
            std::string full_path = node_files_dir + "/" + entry->d_name;
            
            // Check if the entry is a regular file
            if (stat(full_path.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
                files.insert(entry->d_name); // Add the file name to the set
            }
        }
    
        closedir(dir); // Close the directory
        return files; // Return the set of file names
    }



    // Network Communication Functions
    // Function to send a data segment to a specified address using a UDP socket
    static bool send_segment(int send_socket, const std::vector<char>& data, const std::pair<std::string, int>& addr) {
        // Validate the socket
        if (send_socket < 0) {
            log(node_id, "Error: Invalid socket in send_segment.");
            return false; // Return false if the socket is invalid
        }
    
        // Validate the data size
        if (data.empty()) {
            log(node_id, "Error: Empty data vector in send_segment.");
            return false; // Return false if the data vector is empty
        }
    
        // Get local socket information (optional, used for logging or segment metadata)
        struct sockaddr_in sock_addr;
        socklen_t addr_len = sizeof(sock_addr);
        if (getsockname(send_socket, (struct sockaddr*)&sock_addr, &addr_len) == -1) {
            perror("getsockname failed"); // Log the error if getsockname fails
            log(node_id, "Error: Failed to get socket name.");
            return false; // Return false if socket name retrieval fails
        }
    
        // Create a UDP segment (optional, can be used for additional metadata or logging)
        UDPSegment segment(ntohs(sock_addr.sin_port), addr.second, data);
    
        // Prepare the destination address structure
        sockaddr_in client_addr;
        memset(&client_addr, 0, sizeof(client_addr)); // Zero out the structure
        client_addr.sin_family = AF_INET; // Set the address family to IPv4
        client_addr.sin_port = htons(addr.second); // Set the destination port
        client_addr.sin_addr.s_addr = inet_addr(addr.first.c_str()); // Set the destination IP address
    
        // Send the data using the sendto function
        ssize_t sent_len = sendto(send_socket, data.data(), data.size(), 0,
                                  (struct sockaddr*)&client_addr, sizeof(client_addr));
        if (sent_len < 0) {
            perror("sendto failed"); // Log the error if sendto fails
            log(node_id, "Error: Failed to send data to " + addr.first + ":" + std::to_string(addr.second));
            return false; // Return false if data transmission fails
        }
    
        return true; // Return true if the data was successfully sent
    }


    // Tracker Communication Functions
    // Function to register the node in the torrent system by informing the tracker
    void enter_torrent() {
        // Create a message to register the node with the tracker
        Node2Tracker msg(node_id, Config::TrackerRequestsMode::REGISTER, "");

        // Send the registration message to the tracker
        send_segment(send_socket, msg.encode(), {Config::Constants::TRACKER_IP, Config::Constants::TRACKER_PORT});

        // Set a timeout for receiving the acknowledgment (ACK) from the tracker
        struct timeval timeout;
        timeout.tv_sec = 2;  // Timeout duration: 2 seconds
        timeout.tv_usec = 0;
        setsockopt(send_socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

        // Buffer to store the received ACK
        char buffer[Config::Constants::BUFFER_SIZE];
        sockaddr_in from_addr; // Address of the sender
        socklen_t from_len = sizeof(from_addr); // Length of the sender's address structure

        // Attempt to receive the ACK from the tracker
        ssize_t recv_len = recvfrom(send_socket, buffer, sizeof(buffer), 0,
                                    (struct sockaddr*)&from_addr, &from_len);

        // Check if an ACK was received
        if (recv_len > 0) {
            // Convert the received data into a string
            std::string ack(buffer, buffer + recv_len);

            // Verify if the received message is an ACK
            if (ack == "ACK") {
                log(node_id, "ACK received from Tracker"); // Log successful registration
            } else {
                log(node_id, "Unexpected message instead of ACK: " + ack); // Log unexpected message
            }
        } else {
            // Log a timeout or failure to receive ACK
            log(node_id, "ACK not received within timeout. Tracker might be down.");
            return;
        }

        // Log successful entry into the torrent system
        log(node_id, "Entered Torrent.");
    }

    // Function to gracefully exit the torrent system
    void exit_torrent() {
        // Create a message to notify the tracker that this node is exiting
        Node2Tracker msg(node_id, Config::TrackerRequestsMode::EXIT, "");

        // Send the exit message to the tracker
        send_segment(send_socket, msg.encode(), {Config::Constants::TRACKER_IP, Config::Constants::TRACKER_PORT});

        // Log the exit event
        log(node_id, "You exited the torrent!");
    }

    // Thread function to periodically inform the tracker that the node is still active
    static void* inform_tracker_periodically(void* arg) {
        // Interval between heartbeat messages (in seconds)
        int interval = Config::Constants::NODE_TIME_INTERVAL;

        while (true) {
            // Log a message indicating that the tracker is being informed
            std::string log_contain = "I informed the tracker that I'm still alive in the torrent!";
            log(node_id, log_contain);

            // Create a heartbeat message to notify the tracker
            Node2Tracker msg(node_id, Config::TrackerRequestsMode::HEARTBEAT, "");

            // Send the heartbeat message to the tracker
            send_segment(send_socket, msg.encode(), {Config::Constants::TRACKER_IP, Config::Constants::TRACKER_PORT});

            // Sleep for the specified interval before sending the next heartbeat
            sleep(interval);
        }

        return nullptr; // Return nullptr when the thread exits
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
        
        // Wait for a response from the tracker
        while (true) {
            char buffer[Config::Constants::BUFFER_SIZE]; // Buffer to store incoming data
            sockaddr_in addr; // Address of the sender
            socklen_t addr_len = sizeof(addr); // Length of the sender's address structure

            // Receive data from the tracker
            int bytes_received = recvfrom(temp_sock, buffer, sizeof(buffer), 0, (struct sockaddr*)&addr, &addr_len);
            if (bytes_received > 0) {
                // Decode the received data into a Tracker2Node object
                Tracker2Node result = Tracker2Node::decode(std::vector<char>(buffer, buffer + bytes_received));

                // Return the search result containing file owners
                return result.search_result;
            }
        }
    }
        


    // File Sharing Functions
    // Sends the size of the requested file to the requesting node
    static void send_file_size(Node2Node &result, const sockaddr_in& addr) {
        // Extract the filename from the request
        std::string filename = result.filename;

        // Construct the full file path
        std::string file_path = std::string(Config::Directory::NODE_FILES_DIR) + 
                              "node" + std::to_string(node_id) + "/" + filename;
    
        // Check if the file exists and retrieve its metadata using stat
        struct stat file_stat;
        if (stat(file_path.c_str(), &file_stat) != 0) {
            // Log an error if the file doesn't exist or another error occurs
            log(node_id, "File not found: " + filename + (errno != ENOENT ? " (" + std::string(strerror(errno)) + ")" : ""));
            return;
        }
    
        // Verify that the path corresponds to a regular file
        if (!S_ISREG(file_stat.st_mode)) {
            log(node_id, "Path is not a regular file: " + filename);
            return;
        }
    
        // Retrieve the file size from the stat structure
        int size = file_stat.st_size;
        int dest_node_id = result.src_node_id; // Extract the source node ID from the request
    
        // Create a Node2Node message containing the file size
        Node2Node msg(node_id, dest_node_id, filename, size);

        // Convert the IP address from binary to string format
        std::string ip_str = inet_ntoa(addr.sin_addr);

        // Convert the port from network byte order to host byte order
        int port = ntohs(addr.sin_port);

        // Send the file size to the requesting node
        send_segment(send_socket, msg.encode(), {ip_str, port});
    }

    // Splits a file into chunks based on the specified range
    static std::vector<std::vector<char>> split_file_to_chunks(const std::string& file_path, std::pair<int, int> rng) {
        std::vector<std::vector<char>> chunks; // Vector to store the resulting chunks

        // Open the file in binary mode
        std::ifstream file(file_path, std::ios::binary);
        if (!file) {
            // Log an error if the file cannot be opened
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
            // Log an error if seeking fails
            std::cerr << "Error seeking in file: " << std::strerror(errno) << std::endl;
            return chunks;
        }
    
        // Calculate the size of the chunk to be read
        size_t chunk_size = rng.second - rng.first;
        std::vector<char> buffer(chunk_size); // Buffer to hold the chunk data
    
        // Read the chunk from the file
        file.read(buffer.data(), chunk_size);
        size_t bytes_read = file.gcount(); // Get the number of bytes actually read
        if (bytes_read != chunk_size) {
            // Log an error if the number of bytes read is less than expected
            std::cerr << "Error reading file: only " << bytes_read << " bytes read out of " << chunk_size << std::endl;
            return chunks;
        }
    
        // Split the buffer into smaller pieces based on the configured piece size
        size_t piece_size = Config::Constants::CHUNK_PIECES_SIZE;
        if (piece_size == 0) {
            // Log an error if the piece size is invalid
            std::cerr << "Invalid piece size: must be greater than 0" << std::endl;
            return chunks;
        }
    
        // Iterate through the buffer and create smaller pieces
        for (size_t p = 0; p < buffer.size(); p += piece_size) {
            size_t end = std::min(p + piece_size, buffer.size()); // Calculate the end of the current piece
            chunks.emplace_back(buffer.begin() + p, buffer.begin() + end); // Add the piece to the chunks vector
        }
    
        return chunks; // Return the vector of chunks
    }

    // Sends a chunk of a file to a destination node
    static void send_chunk(const std::string& filename, std::pair<int, int> range, int dest_node_id, int dest_port, const std::string& dest_ip) {
        // Validate the specified range
        if (range.first < 0 || range.second < 0 || range.first > range.second) {
            log(node_id, "Error: Invalid range specified for file " + filename);
            return;
        }
    
        // Construct the full file path
        std::string file_path = std::string(Config::Directory::NODE_FILES_DIR) + 
                              "node" + std::to_string(node_id) + "/" + 
                              filename;
    
        // Verify that the file exists and is accessible
        struct stat file_stat;
        if (stat(file_path.c_str(), &file_stat) != 0 || !S_ISREG(file_stat.st_mode)) {
            log(node_id, "Error: File not found or inaccessible: " + filename + 
                (errno != ENOENT ? " (" + std::string(strerror(errno)) + ")" : ""));
            return;
        }
    
        // Split the file into chunks based on the specified range
        std::vector<std::vector<char>> chunk_pieces = split_file_to_chunks(file_path, range);
        if (chunk_pieces.empty()) {
            log(node_id, "Error: Failed to split file into chunks for " + filename);
            return;
        }
    
        // Create a temporary socket for sending chunks
        static std::mutex socket_mutex;
        int temp_port, temp_sock;
        {
            std::lock_guard<std::mutex> lock(socket_mutex);
            temp_port = generate_random_port(); // Generate a random port
            temp_sock = set_socket(temp_port); // Create and bind the socket
        }
        if (temp_sock < 0) {
            log(node_id, "Error: Failed to create temporary socket.");
            return;
        }
    
        // Send each chunk to the destination node
        for (size_t idx = 0; idx < chunk_pieces.size(); ++idx) {
            // Validate the size of the chunk
            if (chunk_pieces[idx].size() > Config::Constants::CHUNK_PIECES_SIZE) {
                log(node_id, "Error: Chunk " + std::to_string(idx) + " exceeds maximum size for file " + filename);
                return;
            }

            // Create a ChunkSharing message for the current chunk
            ChunkSharing msg(node_id, dest_node_id, filename, range, idx, chunk_pieces[idx]);

            // Send the chunk to the destination node
            if (!send_segment(temp_sock, msg.encode(), {dest_ip, dest_port})) {
                log(node_id, "Error: Failed to send chunk " + std::to_string(idx) + " for file " + filename);
                return;
            }

            // Log the successful transmission of the chunk
            log(node_id, "Sent chunk " + std::to_string(idx) + "/" + std::to_string(chunk_pieces.size()) + " for file " + filename);
        }
    
        // Send a termination signal to indicate the end of transmission
        ChunkSharing msg(node_id, dest_node_id, filename, range, -1, {});
        if (!send_segment(temp_sock, msg.encode(), {dest_ip, dest_port})) {
            log(node_id, "Error: Failed to send termination signal for file " + filename);
            return;
        }
    
        // Log the completion of chunk transmission
        log(node_id, "Finished sending chunks for file: " + filename + " to Node " + std::to_string(dest_node_id));
    
        // Notify the tracker about the file update
        Node2Tracker to_tracker_msg(node_id, Config::TrackerRequestsMode::UPDATE, filename);
        if (!send_segment(temp_sock, to_tracker_msg.encode(), {Config::Constants::TRACKER_IP, Config::Constants::TRACKER_PORT})) {
            log(node_id, "Error: Failed to notify tracker about file " + filename);
        }
    }

    // Handles incoming requests from other nodes
    static void handle_requests(char buffer[], int bytes_received, const sockaddr_in& addr) {
        // Decode the received message into a map of properties
        std::unordered_map<std::string, std::any> properties = Message::decode(std::vector<char>(buffer, buffer + bytes_received));
        
        // Extract the filename from the decoded properties
        std::string filename = std::any_cast<std::string>(properties.at("filename"));

        // Check if the message contains a "size" property
        if (properties.find("size") != properties.end()) {
            // Decode the message as a Node2Node message
            Node2Node result = Node2Node::decode(std::vector<char>(buffer, buffer + bytes_received));

            int size = result.size;
            if (size == -1) {
                // If size is -1, it indicates a request for the file size
                log(node_id, "Received a request for size of " + filename);
                send_file_size(result, addr); // Respond with the file size
            }
        } 
        // Check if the message contains a "range_start" property
        else if (properties.find("range_start") != properties.end()) {
            // Decode the message as a ChunkSharing message
            ChunkSharing result = ChunkSharing::decode(std::vector<char>(buffer, buffer + bytes_received));

            // Extract the range and source node ID from the message
            std::pair<int, int> range = result.range;
            int dest_node_id = result.src_node_id;

            // Convert the sender's IP address from binary to string format
            char ipStr[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &(addr.sin_addr), ipStr, INET_ADDRSTRLEN);
            std::string dest_IP_addr(ipStr);  // Store IP as a C++ string

            // Convert the sender's port from network byte order to host byte order
            int dest_port = ntohs(addr.sin_port);

            // Extract the chunk data from the message
            std::vector<char> chunk = result.chunk;

            // If the chunk is empty, it indicates a request for the chunk
            if (chunk.empty()) {
                send_chunk(filename, range, dest_node_id, dest_port, dest_IP_addr); // Send the requested chunk
            }
        }
    }

    // Thread function to listen for incoming requests from other nodes
    static void* listen(void* arg) {
        while (true) {
            char buffer[Config::Constants::BUFFER_SIZE]; // Buffer to store incoming data
            sockaddr_in addr; // Address of the sender
            socklen_t addr_len = sizeof(addr); // Length of the sender's address structure

            // Receive data from the socket
            int bytes_received = recvfrom(send_socket, buffer, sizeof(buffer), 0, (struct sockaddr*)&addr, &addr_len);

            // If data is received, handle the request
            if (bytes_received > 0) {
                handle_requests(buffer, bytes_received, addr); // Process the received request
            }
        }
        return nullptr; // Return nullptr when the thread exits
    }

    // Sets the node to send mode for the specified file
    static void set_send_mode(const std::string& filename) {
        // Check if the file exists in the node's owned files
        if (files.find(filename) == files.end()) {
            log(node_id, "You don't have " + filename); // Log an error if the file is not found
            return;
        }

        // Notify the tracker that this node owns the file
        Node2Tracker msg(node_id, Config::TrackerRequestsMode::OWN, filename);
        send_segment(send_socket, msg.encode(), {Config::Constants::TRACKER_IP, Config::Constants::TRACKER_PORT});  

        // Log the successful registration of the file
        log(node_id, "FILE ENTRY REGISTERED! You are waiting for other nodes' requests!"); // Log the status
        
        // Start a listener thread to handle incoming requests
        pthread_t listener_thread;
        pthread_create(&listener_thread, nullptr, listen, nullptr);
        pthread_detach(listener_thread); // Detach the thread to allow it to run independently

    }



    // File Download Functions
    // Thread function to set the node in download mode for a specific file
    static void* set_download_mode(void* arg) {
        // Extract the filename from the argument
        std::string filename = *((std::string*)arg);

        // Construct the full file path for the file in the node's directory
        std::string file_path = std::string(Config::Directory::NODE_FILES_DIR) + "node" + std::to_string(node_id) + "/" + filename;

        // Check if the file already exists in the node's directory
        struct stat buffer;
        if (stat(file_path.c_str(), &buffer) == 0) {
            // Log a message if the file is already present
            log(node_id, "You already have this file!");
            return nullptr; // Exit the function as the file is already downloaded
        } else {
            // Log a message indicating the start of the search for the file in the torrent
            log(node_id, "Let's search " + filename + " in the torrent!");

            // Search the torrent system for nodes that have the requested file
            std::vector<std::pair<FileOwner, int>> file_owners = search_torrent(filename);

            // If no nodes have the file, log a message and exit
            if (file_owners.empty()) {
                log(node_id, "No one has " + filename);
                return nullptr;
            }

            // Split the file among the available file owners and download it
            split_file_owners(file_owners, filename);
        }
        return nullptr; // Return nullptr when the thread exits
    }

    // Function to request and retrieve the size of a file from a specific file owner
    static int ask_file_size(const std::string& filename, const FileOwner& owner) {
        // Generate a temporary port and socket for communication
        int temp_port = generate_random_port();
        int temp_sock = set_socket(temp_port);
    
        // Create a Node2Node message to request the file size (size = -1 indicates a request)
        Node2Node msg(node_id, owner.node_id, filename, -1);
        send_segment(temp_sock, msg.encode(), {owner.addr.first, owner.addr.second});
    
        // Wait for a response from the file owner
        while (true) {
            char buffer[Config::Constants::BUFFER_SIZE]; // Buffer to store incoming data
            sockaddr_in addr; // Address of the sender
            socklen_t addr_len = sizeof(addr); // Length of the sender's address structure

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

    // Function to split the file among available file owners and download it
    static void split_file_owners(std::vector<std::pair<FileOwner, int>>& file_owners, const std::string& filename) {
        // Filter out the current node from the list of file owners
        std::vector<std::pair<FileOwner, int>> owners;
        for (const auto& owner : file_owners) {
            // Skip the current node (node_id is excluded from owners)
            if (owner.first.node_id != node_id) {
                owners.push_back(owner);
            }
        }
    
        // If no other node has the file, log the message and return
        if (owners.empty()) {
            log(node_id, "No one has " + filename);
            return;
        }
    
        // Sort owners by their send frequency (descending order)
        std::sort(owners.begin(), owners.end(), [](std::pair<FileOwner, int>& a, std::pair<FileOwner, int>& b) {
            return a.second > b.second;  // Sort by frequency of sending chunks
        });
    
        // Select the top file owners (up to MAX_SPLITTNES_RATE)
        std::vector<FileOwner> to_be_used_owners;
        for (int i = 0; i < std::min((int)owners.size(), Config::Constants::MAX_SPLITTNES_RATE); i++) {
            to_be_used_owners.push_back(owners[i].first);  // Add the owner with highest send frequency
        }
    
        // Log the nodes we will download the file from
        std::string log_content = "Downloading " + filename + " from nodes: ";
        for (const auto& owner : to_be_used_owners) {
            log_content += std::to_string(owner.node_id) + " ";  // Append each node_id to the log
        }
        log(node_id, log_content);
    
        // Request the file size from the first peer (to determine the file size for splitting)
        int file_size = ask_file_size(filename, to_be_used_owners[0]);
        log(node_id, "File " + filename + " size: " + std::to_string(file_size) + " bytes");
    
        // Split the file equally among the selected peers
        int step = file_size / to_be_used_owners.size();  // Size of each chunk
        int remainder = file_size % to_be_used_owners.size();  // Remainder to be distributed
        std::vector<std::pair<int, int>> chunks_ranges;  // To store chunk ranges (start, end)
        for (int i = 0; i < to_be_used_owners.size(); i++) {
            int start = step * i;
            int end = (i == to_be_used_owners.size() - 1) ? start + step + remainder : start + step;
            chunks_ranges.emplace_back(start, end);  // Define the chunk range for each owner
        }
    
        // Lock mutex to ensure thread-safety when modifying the downloaded_files map
        static std::mutex downloaded_files_mutex;
        {
            std::lock_guard<std::mutex> lock(downloaded_files_mutex);
            downloaded_files[filename] = {};  // Initialize the map for this file
        }
    
        // Create threads to download chunks concurrently
        std::vector<pthread_t> threads(to_be_used_owners.size());
        for (size_t i = 0; i < to_be_used_owners.size(); i++) {
            // Prepare arguments for each thread (filename, chunk range, and owner)
            auto args = new std::tuple<std::string, std::pair<int, int>, FileOwner>(filename, chunks_ranges[i], to_be_used_owners[i]);
            
            // Create a new thread to download the chunk
            if (pthread_create(&threads[i], nullptr, receive_chunk, args) != 0) {
                log(node_id, "Error: Failed to create thread for chunk " + std::to_string(i));
                delete args;  // Free memory if thread creation fails
            }
        }
    
        // Wait for all threads to finish downloading their respective chunks
        for (auto& thread : threads) {
            pthread_join(thread, nullptr);
        }
    
        // Log the completion of chunk downloading and proceed to sort the chunks
        log(node_id, "All chunks of " + filename + " downloaded. Sorting them now...");
    
        // Sort the downloaded chunks based on their ranges
        std::vector<ChunkSharing> sorted_chunks = sort_downloaded_chunks(filename);
        log(node_id, "All chunks sorted. Reassembling file...");
    
        // Reassemble the file from sorted chunks
        std::string file_path = std::string(Config::Directory::NODE_FILES_DIR) + 
                                "node" + std::to_string(node_id) + "/" + filename;
    
        // Ensure the directory for saving the file exists
        std::string dir_path = std::string(Config::Directory::NODE_FILES_DIR) + 
                               "node" + std::to_string(node_id);
        struct stat st;
        if (stat(dir_path.c_str(), &st) != 0) {
            if (mkdir(dir_path.c_str(), 0755) != 0) {
                log(node_id, "Error creating directory: " + dir_path + " (" + strerror(errno) + ")");
                return;  // Return if directory creation fails
            }
        }
    
        // Reassemble the file using the sorted chunks and save it
        reassemble_file(sorted_chunks, file_path);
        log(node_id, filename + " successfully downloaded and saved.");
    
        // Lock mutex to ensure thread-safety when modifying the 'files' set
        static std::mutex files_mutex;
        {
            std::lock_guard<std::mutex> lock(files_mutex);
            files.insert(filename);  // Mark the file as downloaded
        }
    
        // Inform the tracker that this node now has the file
        set_send_mode(filename);
    }    
    
    // Reassemble the full file from its received chunks and write to disk
    static void reassemble_file(std::vector<ChunkSharing>& chunks, const std::string& file_path) {
        if (chunks.empty()) {
            log(node_id, "Error: No chunks provided for reassembly");
            return;
        }

        // Open the output file in binary write mode, truncating any existing content
        std::ofstream file(file_path, std::ios::binary | std::ios::out | std::ios::trunc);
        if (!file) {
            log(node_id, "Failed to open file: " + file_path + " while assembling");
            return;
        }

        // Write each chunk to the file
        for (const auto& chunk : chunks) {
            if (!file.write(chunk.chunk.data(), chunk.chunk.size())) {
                log(node_id, "Error: Failed to write chunk to file " + file_path);
                return;
            }
        }

        chunks.clear(); // Clear chunk vector after writing

        // Ensure all data is flushed to disk
        if (!file.flush()) {
            log(node_id, "Error: Failed to flush file " + file_path);
            return;
        }

        file.close(); // Close the file

        log(node_id, "File successfully reassembled: " + file_path);
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
            log(node_id, "Error: Failed to create temporary socket");
            return nullptr;
        }

        // Send a request message for the chunk (idx = -1 indicates request)
        ChunkSharing msg(node_id, dest_node_id, filename, range, -1);
        if (!send_segment(temp_sock, msg.encode(), {file_owner.addr.first, file_owner.addr.second})) {
            log(node_id, "Error: Failed to send request for chunk of " + filename + " to node " + std::to_string(dest_node_id));
            free_socket(temp_sock);
            return nullptr;
        }

        // Log the outgoing request
        log(node_id, "I sent a request for a chunk of " + filename + " for node " + std::to_string(dest_node_id));

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
            {
                std::lock_guard<std::mutex> lock(download_mutex);
                downloaded_files[filename].push_back(result);
            }

            retry_count = 0; // Reset retries after a successful receive
        }

        // If retries exceeded, log an error and exit
        log(node_id, "Error: Maximum retries reached for receiving chunks of " + filename);
        free_socket(temp_sock);
        return nullptr;
    }

    // Sorts and returns the downloaded chunks of a file based on range and chunk index
    static std::vector<ChunkSharing> sort_downloaded_chunks(const std::string& filename) {
        // Check if the file has any downloaded chunks
        if (downloaded_files.find(filename) == downloaded_files.end()) {
            log(node_id, "No downloaded chunks found for " + filename);
            return {};
        }

        auto& chunks = downloaded_files[filename];

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
            std::cout << "No owners found for file: " << filename << std::endl;
        } else {
            std::cout << "Owners of file " << filename << ":" << std::endl;
            for (const auto& owner : file_owners) {
                std::cout << "Node " << owner.first.node_id << " (" << owner.first.addr.first << ":" << owner.first.addr.second << ")" << std::endl;
            }
        }
    }

};

#endif