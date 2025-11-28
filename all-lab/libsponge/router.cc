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
    // 提取 dst（已是 uint32_t，无需 .ipv4_numeric()）
    const uint32_t dest_num = dgram.header().dst;

    // 查找最佳路由
    size_t best_length = 0;
    Route best_route;
    bool route_found = false;

    for (const auto &r : _routes) {
        // 修复掩码：用 uint64_t 避免 <<32 UB
        const uint64_t mask64 = 0xFFFFFFFFULL << (32 - r.prefix_length);
        const uint32_t mask = static_cast<uint32_t>(mask64);

        // 先检查匹配，再比较长度（选最长）
        if ((dest_num & mask) == (r.prefix & mask)) {
            if (!route_found || r.prefix_length > best_length) {
                best_length = r.prefix_length;
                best_route = r;
                route_found = true;
            }
        }
    }

    std::cerr << "DEBUG: best_length = " << best_length << ", best_prefix = " << std::hex << best_route.prefix << std::dec << std::endl;

    // 无任何匹配路由，丢弃
    if (!route_found) {
        return;
    }

    // TTL 检查 & 递减
    if (dgram.header().ttl == 0) {
        return;
    }
    dgram.header().ttl--;
    if (dgram.header().ttl == 0) {
        return;
    }

    // next_hop：路由指定或 dst 本身
    const Address next_hop_addr = best_route.next_hop.value_or(
        Address::from_ipv4_numeric(dest_num)
    );

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