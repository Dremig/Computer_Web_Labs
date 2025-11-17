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

    _routes.emplace_back(route_prefix, prefix_length, next_hop, interface_num);
    // DUMMY_CODE(route_prefix, prefix_length, next_hop, interface_num);
    // Your code here.
}

//! \param[in] dgram The datagram to be routed
void Router::route_one_datagram(InternetDatagram &dgram) {
    // 提取数值 dest（用于匹配和 next_hop）
    const uint32_t dest_num = dgram.header().dst;

    std::cerr << "DEBUG: dest_num = " << std::hex << dest_num << std::dec << " (expected ~0x01020304 for 1.2.3.4)" << std::endl;
    // ... 循环 ...


    // 查找最佳路由
    size_t best_length = 0;
    Route best_route;  // 默认构造（prefix=0，确保不匹配）
    for (const auto &r : _routes) {
        const uint32_t mask = 0xFFFFFFFFu << (32 - r.prefix_length);
        if ((dest_num & mask) == (r.prefix & mask) && r.prefix_length > best_length) {
            best_length = r.prefix_length;
            best_route = r;
        }
    }
    std::cerr << "DEBUG: best_length = " << best_length << ", best_prefix = " << std::hex << best_route.prefix << std::dec << std::endl;

    if (best_length == 0) { return; }  // 无匹配，丢弃

    if (dgram.header().ttl == 0) { return; }  // TTL=0，丢弃
    dgram.header().ttl--;
    if (dgram.header().ttl == 0) { return; }  // 减后=0，丢弃

    // next_hop：优先路由的 next_hop，否则用 dest
    const Address next_hop_addr = best_route.next_hop.value_or(Address::from_ipv4_numeric(dest_num));

    // 发送
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