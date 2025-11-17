#include "router.hh"

#include <iostream>

using namespace std;

// Dummy implementation of an IP router

// Given an incoming Internet datagram, the router decides
// (1) which interface to send it out on, and
// (2) what next hop address to send it to.

// For Lab 6, please replace with a real implementation that passes the
// automated checks run by `make check_lab6`.

// You will need to add private members to the class declaration in `router.hh`

template <typename... Targs>
void DUMMY_CODE(Targs &&... /* unused */) {}

//! \param[in] route_prefix The "up-to-32-bit" IPv4 address prefix to match the datagram's destination address against
//! \param[in] prefix_length For this route to be applicable, how many high-order (most-significant) bits of the route_prefix will need to match the corresponding bits of the datagram's destination address?
//! \param[in] next_hop The IP address of the next hop. Will be empty if the network is directly attached to the router (in which case, the next hop address should be the datagram's final destination).
//! \param[in] interface_num The index of the interface to send the datagram out on.
void Router::add_route(const uint32_t route_prefix,
                       const uint8_t prefix_length,
                       const optional<Address> next_hop,
                       const size_t interface_num) {
    cerr << "DEBUG: adding route " << Address::from_ipv4_numeric(route_prefix).ip() << "/" << int(prefix_length)
         << " => " << (next_hop.has_value() ? next_hop->ip() : "(direct)") << " on interface " << interface_num << "\n";

    _routes.push_back({route_prefix, prefix_length, next_hop, interface_num});
    // DUMMY_CODE(route_prefix, prefix_length, next_hop, interface_num);
    // Your code here.
}

//! \param[in] dgram The datagram to be routed
void Router::route_one_datagram(InternetDatagram &dgram) {
    // Get the destination IP address from the datagram header
    const uint32_t dest = dgram.header().dst.ipv4_numeric();

    // Find the best (longest prefix) match in the routing table
    size_t best_length = 0;
    Route best_route;
    for (const auto &r : _routes) {
        // Compute the mask for this prefix length
        const uint32_t mask = 0xFFFFFFFFu << (32 - r.prefix_length);
        // Check if the destination matches this route
        if ((dest & mask) == (r.prefix & mask) && r.prefix_length > best_length) {
            best_length = r.prefix_length;
            best_route = r;
        }
    }

    // If no matching route, drop the datagram (do nothing)
    if (best_length == 0) {
        return;
    }

    // Check TTL: if already 0, drop
    if (dgram.header().ttl == 0) {
        return;
    }

    // Decrement TTL
    dgram.header().ttl--;

    // If TTL now 0, drop
    if (dgram.header().ttl == 0) {
        return;
    }

    // Determine the next hop address: use the route's next_hop if present, otherwise the destination itself (direct route)
    const Address next_hop_addr = best_route.next_hop.value_or(Address::from_ipv4_numeric(dest));

    // Send the modified datagram via the appropriate interface
    _interfaces.at(best_route.interface_num).send_datagram(dgram, next_hop_addr);
}

void Router::route() {
    // Go through all the interfaces, and route every incoming datagram to its proper outgoing interface.
    for (auto &interface : _interfaces) {
        auto &queue = interface.datagrams_out();
        while (not queue.empty()) {
            route_one_datagram(queue.front());
            queue.pop();
        }
    }
}