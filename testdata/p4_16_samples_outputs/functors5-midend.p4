parser p2_0(out bit<2> w) {
    @name("w_0") bit<2> w_1;
    state start {
        w_1 = 2w2;
        w = w_1;
        transition accept;
    }
}

parser simple(out bit<2> w);
package m(simple n);
m(p2_0()) main;
