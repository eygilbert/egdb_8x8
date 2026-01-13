#pragma once
#include <tuple>

class Slice {
    static const int min_ncolor = 1;
    static const int max_ncolor = 7;

    int npieces_;
    int nb_;
    int nw_;
    int nbm_;
    int nbk_;
    int nwm_;
    int nwk_;

    void assert_invariants() const;

public:
    // first slice of <n> pieces
    explicit Slice(int n)
    {
        *this = Slice( (n + 1) / 2, n - (n + 1) / 2 );
    }

    // first slice of <b, w> pieces
    Slice(int b, int w)
    :
        npieces_( b + w ),
        nb_( b ),
        nw_( w ),
        nbm_( 0 ),
        nbk_( nb_ ),
        nwm_( 0 ),
        nwk_( nw_)
    {
        assert_invariants();
    }

    // slice of exactly <bm, bk, wm, wk> pieces
    Slice(int bm, int bk, int wm, int wk)
    :
        npieces_( bm + bk + wm + wk ),
        nb_( bm + bk ),
        nw_( wm + wk ),
        nbm_( bm ),
        nbk_( bk ),
        nwm_( wm ),
        nwk_( wk )
    {
        assert_invariants();
    }

    int increment();

    int npieces() const { return npieces_; }
    int nb()      const { return nb_; }
    int nw()      const { return nw_; }
    int nbm()     const { return nbm_; }
    int nbk()     const { return nbk_; }
    int nwm()     const { return nwm_; }
    int nwk()     const { return nwk_; }

	inline auto as_tuple(void) const
	{ 
		return std::make_tuple(npieces(), nb(), nbk(), nwk()); 
	}  

};


inline bool operator <(const Slice &s1, const Slice &s2)
{       
	return s1.as_tuple() < s2.as_tuple();
}