#define _Alignof(x) __alignof__(x)
#include <iostream>

#include "node.h"

int Node::node_id;
int Node::send_socket;
std::unordered_set<std::string> Node::files;
std::unordered_map<std::string, std::vector<ChunkSharing>> Node::downloaded_files;

// Main function to start the node with given node_id
void run(int node_id) {
    Node node;

    // Initialize the node with the given node_id and randomly generated ports
    node.init(node_id, generate_random_port(), generate_random_port());

    std::string log_content = "********** Node program started just right now! **********";
    Node::log_thread_safe(node_id, log_content);

    // Register the node in the torrent system by informing the tracker
    node.enter_torrent();
    
    // Start a background thread that periodically informs the tracker
    pthread_t timer_thread;
    pthread_create(&timer_thread, nullptr, node.inform_tracker_periodically, nullptr);
    pthread_detach(timer_thread); 

    std::cout << "********** ENTER YOUR COMMAND! **********" << std::endl;
    std::cout << "Available commands: send <filename>, download <filename>, search <filename>, exit" << std::endl;

    std::string command;
    while (true) {
        std::getline(std::cin, command); 

        // Parse the command into <mode> and <filename>
        std::pair<std::string, std::string> cmd = parse_command(command);
        std::string mode = cmd.first;
        std::string filename = cmd.second;
        
        if (mode == "send") {
            // Refresh the list of files owned by this node
            node.files = Node::fetch_owned_files();

            // Prepare to serve the requested file to peers
            node.set_send_mode(filename);

        } else if (mode == "download") {
            // Start a new thread to handle downloading the specified file
            pthread_t timer_thread;
            pthread_create(&timer_thread, nullptr, node.set_download_mode, &filename);
            pthread_detach(timer_thread);

        } else if (mode == "search") {
            // Search for nodes that have the specified file
            node.search_file_owners(filename);

        } else if (mode == "exit") {
            // Gracefully exit the torrent: notify tracker and close node
            node.exit_torrent();
            exit(0);

        } else {
            // Invalid command entered
            std::cout << "Invalid command. Available commands: send <filename>, download <filename>, search <filename>, exit" << std::endl;
        }
    }
}


int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <node_id>" << std::endl;
        return 1;
    }
    // Convert the node_id from string to integer
    int node_id = std::stoi(argv[1]);

    // Run the node with the specified node_id
    run(node_id);
    return 0;
}
