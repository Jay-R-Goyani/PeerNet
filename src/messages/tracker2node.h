#ifndef TRACKER2NODE_H
#define TRACKER2NODE_H

#include "message.h"
#include <string>
#include <vector>
#include <sstream>

/**
 * @struct FileOwner
 * @brief Structure representing a node that owns a file
 * 
 * Contains identifying information about a node including its ID and network address
 */
struct FileOwner {
    int node_id;                         // Unique identifier for the node
    std::pair<std::string, int> addr;    // Network address as (IP, Port) pair
};

/**
 * @class Tracker2Node
 * @brief Message class for tracker responses to node requests
 * 
 * Handles communication from tracker to nodes, primarily for responding to file search requests
 * by providing information about nodes that have the requested file.
 */
class Tracker2Node : public Message {
public:
    int dest_node_id;                                        // ID of destination node
    std::vector<std::pair<FileOwner, int>> search_result;   // List of file owners(IP and PORT) and their frequency
    std::string filename;                                    // Name of requested file

    /**
     * @brief Constructor for Tracker2Node message
     * @param dest Destination node ID
     * @param result Vector of file owners and their frequencies
     * @param file Name of the requested file
     */
    Tracker2Node(int dest, const std::vector<std::pair<FileOwner, int>>& result, const std::string& file)
        : dest_node_id(dest), search_result(result), filename(file) {
        set_property("dest_node_id", std::to_string(dest_node_id));
        set_property("filename", filename);
        set_property("search_result", serialize_result(result));
    }

    /**
     * @brief Serializes search results into a string format
     * @param result Vector of FileOwner and frequency pairs
     * @return String representation in format "(node_id,ip,port):frequency;"
     */
    static std::string serialize_result(const std::vector<std::pair<FileOwner, int>>& result) {
        std::ostringstream oss;
        for (const auto& entry : result) {
            const FileOwner& owner = entry.first;
            oss << "(" << owner.node_id << ","
                << owner.addr.first << ","
                << owner.addr.second << ")"
                << ":" << entry.second << ";";  // Format: (node_id,ip,port):frequency;
        }
        return oss.str();
    }

    /**
     * @brief Deserializes a string back into search results
     * @param serialized String in format "(node_id,ip,port):frequency;"
     * @return Vector of FileOwner and frequency pairs
     * 
     * Parses the serialized string format and reconstructs the original data structure.
     * Handles malformed input gracefully by skipping invalid entries.
     */
    static std::vector<std::pair<FileOwner, int>> deserialize_result(const std::string& serialized) {
        std::vector<std::pair<FileOwner, int>> result;
        std::istringstream iss(serialized);
        std::string part;

        while (std::getline(iss, part, ';')) {  // Split by entry separator
            if (part.empty()) continue;

            size_t colon_pos = part.find(':');
            if (colon_pos == std::string::npos) continue;

            std::string node_str = part.substr(0, colon_pos);  // Get node info portion
            int freq = std::stoi(part.substr(colon_pos + 1));  // Get frequency

            // Parse node information
            size_t first_comma = node_str.find(',');
            size_t last_comma = node_str.rfind(',');

            if (first_comma == std::string::npos || last_comma == std::string::npos || first_comma == last_comma) continue;

            // Extract individual components
            int node_id = std::stoi(node_str.substr(1, first_comma - 1));
            std::string ip = node_str.substr(first_comma + 1, last_comma - first_comma - 1);
            int port = std::stoi(node_str.substr(last_comma + 1, node_str.size() - last_comma - 2));

            result.emplace_back(FileOwner{node_id, {ip, port}}, freq);
        }
        return result;
    }

    /**
     * @brief Serializes the message into binary format
     * @return Vector of bytes representing the encoded message
     */
    std::vector<char> encode() const {
        return Message::encode();
    }

    /**
     * @brief Reconstructs a Tracker2Node object from binary data
     * @param data Binary data produced by encode()
     * @return Reconstructed Tracker2Node object
     */
    static Tracker2Node decode(const std::vector<char>& data) {
        auto properties = Message::decode(data);

        int dest = std::stoi(std::any_cast<std::string>(properties["dest_node_id"]));
        std::string filename = std::any_cast<std::string>(properties["filename"]);
        std::vector<std::pair<FileOwner, int>> result = deserialize_result(
            std::any_cast<std::string>(properties["search_result"])
        );

        return Tracker2Node(dest, result, filename);
    }
};

#endif // TRACKER2NODE_H
