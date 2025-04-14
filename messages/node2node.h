#ifndef NODE2NODE_H
#define NODE2NODE_H

#include "message.h"
#include <string>

/**
 * @class Node2Node
 * @brief Message class for direct node-to-node communication
 * 
 * Handles communication between peer nodes in the network, primarily for
 * file transfer coordination and size queries between nodes.
 */
class Node2Node : public Message {
public:
    int src_node_id;        // ID of the source/sending node
    int dest_node_id;       // ID of the destination/receiving node  
    std::string filename;   // Name of the file being transferred/queried
    int size;              // File size in bytes (-1 indicates a size query request)

    /**
     * @brief Constructor for Node2Node message
     * @param src Source node ID
     * @param dest Destination node ID 
     * @param file Name of the file being operated on
     * @param sz Size of file in bytes (-1 for size queries)
     *
     * Creates a new message for node-to-node communication and initializes
     * all required properties for transmission
     */
    Node2Node(int src, int dest, const std::string& file, int sz = -1)
        : src_node_id(src), dest_node_id(dest), filename(file), size(sz) {
        set_property("src_node_id", std::to_string(src_node_id));
        set_property("dest_node_id", std::to_string(dest_node_id));
        set_property("filename", filename);
        set_property("size", std::to_string(size));
    }

    /**
     * @brief Serializes the message into binary format
     * @return Vector of bytes representing the encoded message
     * 
     * Utilizes base Message class encoding functionality to convert
     * the message properties into a transmittable binary format
     */
    std::vector<char> encode() const {
        return Message::encode();
    }

    /**
     * @brief Reconstructs a Node2Node object from binary data
     * @param data Binary data produced by encode()
     * @return Reconstructed Node2Node object
     * 
     * Decodes the binary data and extracts properties to create
     * a new message object with the original values
     */
    static Node2Node decode(const std::vector<char>& data) {
        auto properties = Message::decode(data);
        
        int src = std::stoi(std::any_cast<std::string>(properties["src_node_id"]));
        int dest = std::stoi(std::any_cast<std::string>(properties["dest_node_id"]));
        std::string file = std::any_cast<std::string>(properties["filename"]);
        int sz = std::stoi(std::any_cast<std::string>(properties["size"]));

        return Node2Node(src, dest, file, sz);
    }
};

#endif // NODE2NODE_H
