#ifndef UDP_SEGMENT_H
#define UDP_SEGMENT_H

#include <iostream>
#include <vector>
#include <stdexcept>
#include "config.h"  // Include config file

class UDPSegment {
public:
    int src_port;
    int dest_port;
    size_t length;
    std::vector<char> data;

    UDPSegment(int src_port, int dest_port, const std::vector<char>& data)
        : src_port(src_port), dest_port(dest_port), length(data.size()), data(data) {
        // Validate data size
        if (length > Config::Constants::MAX_UDP_SEGMENT_DATA_SIZE) {
            throw std::runtime_error("MAXIMUM DATA SIZE OF A UDP SEGMENT EXCEEDED!");
        }
    }
};

#endif // UDP_SEGMENT_H