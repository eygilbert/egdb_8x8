#include <algorithm>
#include <cassert>
#include "slice.h"


const int Slice::min_ncolor;
const int Slice::max_ncolor;

void Slice::assert_invariants() const {
    assert(nbm() + nbk() == nb());
    assert(nwm() + nwk() == nw());
    assert(nb() + nw_ == npieces());
    assert(min_ncolor <= nb()); assert(nb() <= max_ncolor);
    assert(min_ncolor <= nw()); assert(nw() <= max_ncolor);
}

int Slice::increment()
{
    if (nwk() > 0) {
        ++nwm_;
        --nwk_;
        assert_invariants();
        return(npieces_);
    }

    if (nbk() > 0) {
        ++nbm_;
        --nbk_;
        if (nb() == nw()) {
            nwm_ = nbm();
            nwk_ = nbk();
        }
        else {
            nwk_ = nw();
            nwm_ = 0;
        }
        assert_invariants();
        return(npieces_);
    }

    if (nb() < (std::min)(npieces() - min_ncolor, max_ncolor)) {
        *this = Slice(nb() + 1, nw() - 1);
        return(npieces_);
    }

    *this = Slice(npieces() + 1);
	return(npieces_);
}

