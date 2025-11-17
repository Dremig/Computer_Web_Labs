#include "network_interface.hh"

#include "arp_message.hh"
#include "ethernet_frame.hh"

#include <iostream>

// Dummy implementation of a network interface
// Translates from {IP datagram, next hop address} to link-layer frame, and from link-layer frame to IP datagram

// You will need to add private members to the class declaration in `network_interface.hh`

template <typename... Targs>
void DUMMY_CODE(Targs &&... /* unused */) {}

using namespace std;

//! \param[in] ethernet_address Ethernet (what ARP calls "hardware") address of the interface
//! \param[in] ip_address IP (what ARP calls "protocol") address of the interface
NetworkInterface::NetworkInterface(const EthernetAddress &ethernet_address, const Address &ip_address)
    : _ethernet_address(ethernet_address), _ip_address(ip_address) {
    cerr << "DEBUG: Network interface has Ethernet address " << to_string(_ethernet_address) << " and IP address "
         << ip_address.ip() << "\n";
}

//! \param[in] dgram the IPv4 datagram to be sent
//! \param[in] next_hop the IP address of the interface to send it to (typically a router or default gateway, but may also be another host if directly connected to the same network as the destination)
//! (Note: the Address type can be converted to a uint32_t (raw 32-bit IP address) with the Address::ipv4_numeric() method.)
void NetworkInterface::send_datagram(const InternetDatagram &dgram, const Address &next_hop) {
    // convert IP address of next hop to raw 32-bit representation (used in ARP header)
    const uint32_t next_hop_ip = next_hop.ipv4_numeric();
    auto it = _arp_table.find(next_hop_ip);
    if (it != _arp_table.end() && it->second.ttl > 0) {
        // find this IP in ARP table, and the entry is still alive, so we simply send the datagram
        EthernetFrame frame;
        frame.header().dst = it->second.ethernet_address;
        frame.header().src = _ethernet_address;
        frame.header().type = EthernetHeader::TYPE_IPv4;
        frame.payload() = dgram.serialize();
        _frames_out.push(frame);
    } else {
        // not found or expired, so we need to send an ARP request
        _pending_datagrams[next_hop_ip].push_back(dgram);
        auto timer_it = _arp_request_timers.find(next_hop_ip);
        // check whether we have already sent an ARP request
        if (timer_it == _arp_request_timers.end() || timer_it->second == 0) {
            ARPMessage arp_request;
            arp_request.opcode = ARPMessage::OPCODE_REQUEST;
            arp_request.sender_ethernet_address = _ethernet_address;
            arp_request.sender_ip_address = _ip_address.ipv4_numeric();
            arp_request.target_ip_address = next_hop_ip;
            EthernetFrame frame;
            frame.header().dst = ETHERNET_BROADCAST;
            frame.header().src = _ethernet_address;
            frame.header().type = EthernetHeader::TYPE_ARP;
            frame.payload() = arp_request.serialize();
            _frames_out.push(std::move(frame));
            // timer set
            _arp_request_timers[next_hop_ip] = 5000;
        }
    }

    // DUMMY_CODE(dgram, next_hop, next_hop_ip);

}

//! \param[in] frame the incoming Ethernet frame
optional<InternetDatagram> NetworkInterface::recv_frame(const EthernetFrame &frame) {
    const EthernetHeader &ethernet_header = frame.header();
    if (ethernet_header.dst != _ethernet_address && ethernet_header.dst != ETHERNET_BROADCAST) {
        // none of our addresses match the destination, so ignore this frame
        return {};
    }
    if (ethernet_header.type == EthernetHeader::TYPE_IPv4) {
        InternetDatagram datagram;
        if (datagram.parse(frame.payload()) == ParseResult::NoError) {
            // successfully parsed the datagram
            return datagram;
        }
    }
    else if (ethernet_header.type == EthernetHeader::TYPE_ARP) {
        ARPMessage arp_message;
        if (arp_message.parse(frame.payload()) == ParseResult::NoError) {
            _arp_table[arp_message.sender_ip_address].ethernet_address = arp_message.sender_ethernet_address;
            _arp_table[arp_message.sender_ip_address].ttl = 30000;
            if (arp_message.opcode == ARPMessage::OPCODE_REQUEST && arp_message.target_ip_address == _ip_address.ipv4_numeric()) {
                ARPMessage arp_reply;
                arp_reply.opcode = ARPMessage::OPCODE_REPLY;
                arp_reply.sender_ethernet_address = _ethernet_address;
                arp_reply.sender_ip_address = _ip_address.ipv4_numeric();
                arp_reply.target_ethernet_address = arp_message.sender_ethernet_address;
                arp_reply.target_ip_address = arp_message.sender_ip_address;
                EthernetFrame rframe;
                rframe.header().dst = arp_message.sender_ethernet_address;
                rframe.header().src = _ethernet_address;
                rframe.header().type = EthernetHeader::TYPE_ARP;
                rframe.payload() = arp_reply.serialize();
                _frames_out.push(std::move(rframe));
            }
            auto it = _pending_datagrams.find(arp_message.sender_ip_address);
            if (it != _pending_datagrams.end()) {
                // exist pending datagrams, so we send them out
                for (const auto &datagram : it->second) {
                    EthernetFrame pframe;
                    pframe.header().dst = arp_message.sender_ethernet_address;
                    pframe.header().src = _ethernet_address;
                    pframe.header().type = EthernetHeader::TYPE_IPv4;
                    pframe.payload() = datagram.serialize();
                    _frames_out.push(std::move(pframe));
                }
                _pending_datagrams.erase(it);
                _arp_request_timers.erase(arp_message.sender_ip_address);
            }
        }
    }
    // DUMMY_CODE(frame);
    return {};
}

//! \param[in] ms_since_last_tick the number of milliseconds since the last call to this method
void NetworkInterface::tick(const size_t ms_since_last_tick) { 
    // arp tables handling
    for (auto it = _arp_table.begin(); it != _arp_table.end();) {
        if (it->second.ttl <= ms_since_last_tick) {
            it = _arp_table.erase(it);
        }
        else {
            it->second.ttl -= ms_since_last_tick;
            ++it;
        }
    }
    // arp request timers handling
    for (auto &[it, timer] : _arp_request_timers) {
        if (timer <= ms_since_last_tick) {
            timer = 0;
        }
        else {
            timer -= ms_since_last_tick;
        }
    }
    for (auto &[ip, pender] : _pending_datagrams) {
        if (pender.empty()) {
            continue;
        }
        auto it = _arp_request_timers.find(ip);
        if (it == _arp_request_timers.end() || it->second == 0) {
            // resend the ARP request
            ARPMessage arp_request;
            arp_request.opcode = ARPMessage::OPCODE_REQUEST;
            arp_request.sender_ethernet_address = _ethernet_address;
            arp_request.sender_ip_address = _ip_address.ipv4_numeric();
            arp_request.target_ip_address = ip;
            EthernetFrame frame;
            frame.header().dst = ETHERNET_BROADCAST;
            frame.header().src = _ethernet_address;
            frame.header().type = EthernetHeader::TYPE_ARP;
            frame.payload() = arp_request.serialize();
            _frames_out.push(std::move(frame));
            // timer set
            _arp_request_timers[ip] = 5000;
        }
    }
    // DUMMY_CODE(ms_since_last_tick); 
}
