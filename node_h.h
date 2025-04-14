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

class Node {
    public:
    static int node_id;                                                                   // Node ID
    static int send_socket;                                                               // Socket for sending messages
    static std::unordered_set<std::string> files;                                         // Set of files owned by this node
    static std::unordered_map<std::string, std::vector<ChunkSharing>> downloaded_files;   // Map of downloaded files and their chunks
    static bool is_in_send_mode;                                                         // Flag to indicate if the node is in send mode

    // Initialize the node with the given ID, receive port, and send port
    static void init(int id, int rcv_port, int send_port) {
        node_id = id; // Set the node ID
        send_socket = set_socket(send_port); // Create and bind the send socket
        files = fetch_owned_files(); // Fetch the list of files owned by this node
        is_in_send_mode = false; // Set the initial mode to not in send mode
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

};

#endif