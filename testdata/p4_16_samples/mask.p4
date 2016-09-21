/*
Copyright 2016 VMware, Inc.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/
#include <core.p4>

header Ipv4_base {
    bit<4> version;
    bit<4> ihl;
    bit<8> diffserv;
    bit<16> totalLen;
    bit<16> identification;
    bit<3> flags;
    bit<13> fragOffset;
    bit<8> ttl;
    bit<8> protocol;
    bit<16> hdrChecksum;
    bit<32> srcAddr;
    bit<32> dstAddr;
}

header Ipv4_option_NOP {
    bit<8> value;
}

struct Parsed_Packet {
    Ipv4_base ipv4;
    Ipv4_option_NOP[3] nop;
}

parser Parser(packet_in b, out Parsed_Packet p) {
    state start {
        transition select(8w0, b.lookahead<bit<8>>()) {
            default : accept;
            (0, 0 &&& 0) : accept;
            (0 &&& 0, 0x44) : ipv4_option_NOP;
        }
    }

    state ipv4_option_NOP {
        b.extract(p.nop.next);
        transition start;
    }

}

package Switch();

Switch() main;
