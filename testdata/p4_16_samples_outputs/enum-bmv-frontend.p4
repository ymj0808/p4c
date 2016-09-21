#include <core.p4>
#include <v1model.p4>

header hdr {
    bit<32> a;
    bit<32> b;
    bit<32> c;
}

enum Choice {
    First,
    Second
}

control compute(inout hdr h) {
    @name("c") Choice c_0;
    bool tmp;
    apply {
        c_0 = Choice.First;
        tmp = c_0 == Choice.Second;
        if (tmp) 
            h.c = h.a;
        else 
            h.c = h.b;
    }
}

struct Headers {
    hdr h;
}

struct Meta {
}

parser p(packet_in b, out Headers h, inout Meta m, inout standard_metadata_t sm) {
    state start {
        b.extract<hdr>(h.h);
        transition accept;
    }
}

control vrfy(in Headers h, inout Meta m, inout standard_metadata_t sm) {
    apply {
    }
}

control update(inout Headers h, inout Meta m, inout standard_metadata_t sm) {
    apply {
    }
}

control egress(inout Headers h, inout Meta m, inout standard_metadata_t sm) {
    apply {
    }
}

control deparser(packet_out b, in Headers h) {
    apply {
        b.emit<hdr>(h.h);
    }
}

control ingress(inout Headers h, inout Meta m, inout standard_metadata_t sm) {
    @name("c") compute() c_1;
    apply {
        c_1.apply(h.h);
        sm.egress_spec = 9w0;
    }
}

V1Switch<Headers, Meta>(p(), vrfy(), ingress(), egress(), update(), deparser()) main;
