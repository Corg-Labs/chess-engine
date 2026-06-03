/*
 * chess.c — Terminal Chess Engine
 * Corg-Labs | gcc chess.c -o chess
 *
 * Human (White) vs Engine (Black).
 * Input: algebraic notation  e2e4  or  e2-e4  (source-square dest-square).
 * Features:
 *   • Full legal move generation for all pieces
 *   • Castling (kingside & queenside) and en passant
 *   • Pawn promotion (always promotes to queen)
 *   • Minimax + alpha-beta pruning, depth 4
 *   • ANSI coloured board with Unicode chess pieces
 *   • Captured pieces sidebar and move history
 *   • Check / checkmate / stalemate detection
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>

/* ── ANSI ─────────────────────────────────────────────────────────────── */
#define RST      "\033[0m"
#define BOLD     "\033[1m"
#define DIM      "\033[2m"
/* board square backgrounds */
#define BG_LIGHT "\033[48;5;223m"   /* light square: warm beige           */
#define BG_DARK  "\033[48;5;136m"   /* dark  square: goldenrod            */
#define BG_HIGH  "\033[48;5;154m"   /* highlight: bright green            */
/* piece colours */
#define FG_WHITE "\033[97;1m"       /* white pieces: bright white + bold  */
#define FG_BLACK "\033[30;1m"       /* black pieces: bold black           */
/* UI colours */
#define CYAN     "\033[1;36m"
#define GREEN    "\033[1;32m"
#define YELLOW   "\033[1;33m"
#define RED      "\033[1;31m"
#define MAGENTA  "\033[1;35m"

/* ── Piece encoding ───────────────────────────────────────────────────── *
 * 0 = empty
 * Positive = White, Negative = Black
 * 1=Pawn 2=Knight 3=Bishop 4=Rook 5=Queen 6=King
 * ─────────────────────────────────────────────────────────────────────── */
#define EMPTY   0
#define W_PAWN  1
#define W_KNIGHT 2
#define W_BISHOP 3
#define W_ROOK  4
#define W_QUEEN 5
#define W_KING  6
#define B_PAWN  (-1)
#define B_KNIGHT (-2)
#define B_BISHOP (-3)
#define B_ROOK  (-4)
#define B_QUEEN (-5)
#define B_KING  (-6)

#define WHITE  1
#define BLACK -1

/* ── Unicode glyphs ───────────────────────────────────────────────────── */
static const char *GLYPH[13] = {
    /* index 0 is unused; 1-6 white, offset+6 black */
    "",
    "♙", "♘", "♗", "♖", "♕", "♔",  /* white 1-6  */
    "♟", "♞", "♝", "♜", "♛", "♚"   /* black 7-12 */
};

static const char *piece_glyph(int p) {
    if (p == EMPTY) return " ";
    if (p > 0) return GLYPH[p];
    return GLYPH[-p + 6];
}

/* ── Move struct ──────────────────────────────────────────────────────── */
typedef struct {
    int from, to;       /* 0-63 board indices (rank*8+file)                */
    int promo;          /* promotion piece (0 = none)                      */
    int castle;         /* 0=none 1=kingside 2=queenside                   */
    int ep;             /* en-passant capture square (-1 = none)           */
} Move;

/* ── Game state ───────────────────────────────────────────────────────── */
typedef struct {
    int  board[64];
    int  side;          /* WHITE or BLACK to move                          */
    int  ep_sq;         /* en-passant target square (-1 = none)            */
    int  castle_rights; /* bits: 0=WK 1=WQ 2=BK 3=BQ                      */
    int  halfmove;      /* halfmove clock (50-move rule)                   */
    int  fullmove;
} State;

/* ── History for display ──────────────────────────────────────────────── */
#define MAX_HIST 200
static char   move_history[MAX_HIST][16];
static int    hist_count = 0;

/* captured pieces */
static int    w_captured[16], wc_count = 0;
static int    b_captured[16], bc_count = 0;

/* last move squares for highlighting */
static int    last_from = -1, last_to = -1;

/* ── Utility ──────────────────────────────────────────────────────────── */
static inline int rank_of(int sq) { return sq >> 3; }
static inline int file_of(int sq) { return sq & 7; }
static inline int sq(int r, int f) { return r * 8 + f; }
static inline int on_board(int r, int f) { return r >= 0 && r < 8 && f >= 0 && f < 8; }
static inline int piece_color(int p) { return (p > 0) ? WHITE : (p < 0 ? BLACK : 0); }

/* ── Initial position ─────────────────────────────────────────────────── */
static void init_state(State *s) {
    memset(s, 0, sizeof *s);
    s->ep_sq = -1;
    s->side  = WHITE;
    s->castle_rights = 0xF;
    s->fullmove = 1;

    int back_row[8] = {W_ROOK,W_KNIGHT,W_BISHOP,W_QUEEN,
                       W_KING,W_BISHOP,W_KNIGHT,W_ROOK};
    for (int f = 0; f < 8; f++) {
        s->board[sq(0,f)] =  back_row[f];
        s->board[sq(1,f)] =  W_PAWN;
        s->board[sq(6,f)] =  B_PAWN;
        s->board[sq(7,f)] = -back_row[f];
    }
}

/* ── Is square attacked by `attacker` side? ─────────────────────────── */
static int is_attacked(const State *s, int target, int attacker) {
    /* Pawn attacks */
    int pd = (attacker == WHITE) ? 1 : -1;  /* direction pawns move */
    int pr = rank_of(target) - pd;
    if (pr >= 0 && pr < 8) {
        for (int df = -1; df <= 1; df += 2) {
            int pf = file_of(target) + df;
            if (pf >= 0 && pf < 8) {
                int p = s->board[sq(pr, pf)];
                if (attacker == WHITE && p == W_PAWN) return 1;
                if (attacker == BLACK && p == B_PAWN) return 1;
            }
        }
    }
    /* Knight */
    static const int kn[8][2] = {{2,1},{2,-1},{-2,1},{-2,-1},
                                   {1,2},{1,-2},{-1,2},{-1,-2}};
    int tr = rank_of(target), tf = file_of(target);
    for (int i = 0; i < 8; i++) {
        int nr = tr + kn[i][0], nf = tf + kn[i][1];
        if (on_board(nr, nf)) {
            int p = s->board[sq(nr, nf)];
            if (attacker == WHITE && p == W_KNIGHT) return 1;
            if (attacker == BLACK && p == B_KNIGHT) return 1;
        }
    }
    /* Sliding: bishop/queen diagonals */
    static const int diag[4][2] = {{1,1},{1,-1},{-1,1},{-1,-1}};
    for (int d = 0; d < 4; d++) {
        int nr = tr + diag[d][0], nf = tf + diag[d][1];
        while (on_board(nr, nf)) {
            int p = s->board[sq(nr, nf)];
            if (p != EMPTY) {
                if (attacker == WHITE && (p == W_BISHOP || p == W_QUEEN)) return 1;
                if (attacker == BLACK && (p == B_BISHOP || p == B_QUEEN)) return 1;
                break;
            }
            nr += diag[d][0]; nf += diag[d][1];
        }
    }
    /* Sliding: rook/queen straights */
    static const int orth[4][2] = {{1,0},{-1,0},{0,1},{0,-1}};
    for (int d = 0; d < 4; d++) {
        int nr = tr + orth[d][0], nf = tf + orth[d][1];
        while (on_board(nr, nf)) {
            int p = s->board[sq(nr, nf)];
            if (p != EMPTY) {
                if (attacker == WHITE && (p == W_ROOK || p == W_QUEEN)) return 1;
                if (attacker == BLACK && (p == B_ROOK || p == B_QUEEN)) return 1;
                break;
            }
            nr += orth[d][0]; nf += orth[d][1];
        }
    }
    /* King */
    for (int dr = -1; dr <= 1; dr++)
    for (int df = -1; df <= 1; df++) {
        if (!dr && !df) continue;
        int nr = tr + dr, nf = tf + df;
        if (on_board(nr, nf)) {
            int p = s->board[sq(nr, nf)];
            if (attacker == WHITE && p == W_KING) return 1;
            if (attacker == BLACK && p == B_KING) return 1;
        }
    }
    return 0;
}

/* ── Find king square ─────────────────────────────────────────────────── */
static int king_sq(const State *s, int color) {
    int k = (color == WHITE) ? W_KING : B_KING;
    for (int i = 0; i < 64; i++)
        if (s->board[i] == k) return i;
    return -1;
}

static int in_check(const State *s, int color) {
    int ksq = king_sq(s, color);
    if (ksq < 0) return 0;
    return is_attacked(s, ksq, -color);
}

/* ── Apply move (returns new state) ─────────────────────────────────── */
static State apply_move(const State *s, Move m) {
    State ns = *s;
    int piece = ns.board[m.from];
    int captured = ns.board[m.to];

    ns.board[m.to]   = piece;
    ns.board[m.from] = EMPTY;
    ns.ep_sq = -1;

    /* En-passant capture */
    if (m.ep >= 0) {
        ns.board[m.ep] = EMPTY;
        captured = (s->side == WHITE) ? B_PAWN : W_PAWN;
    }

    /* Pawn double push: set ep square */
    if ((piece == W_PAWN || piece == B_PAWN)) {
        int dr = rank_of(m.to) - rank_of(m.from);
        if (dr == 2 || dr == -2)
            ns.ep_sq = (m.from + m.to) / 2;
    }

    /* Promotion */
    if (m.promo) ns.board[m.to] = m.promo * s->side;

    /* Castling: move rook */
    if (m.castle == 1) {  /* kingside */
        int r = (s->side == WHITE) ? 0 : 7;
        ns.board[sq(r,5)] = ns.board[sq(r,7)];
        ns.board[sq(r,7)] = EMPTY;
    } else if (m.castle == 2) {  /* queenside */
        int r = (s->side == WHITE) ? 0 : 7;
        ns.board[sq(r,3)] = ns.board[sq(r,0)];
        ns.board[sq(r,0)] = EMPTY;
    }

    /* Update castling rights */
    if (piece == W_KING) ns.castle_rights &= ~0x3;
    if (piece == B_KING) ns.castle_rights &= ~0xC;
    if (m.from == sq(0,0) || m.to == sq(0,0)) ns.castle_rights &= ~0x2;
    if (m.from == sq(0,7) || m.to == sq(0,7)) ns.castle_rights &= ~0x1;
    if (m.from == sq(7,0) || m.to == sq(7,0)) ns.castle_rights &= ~0x8;
    if (m.from == sq(7,7) || m.to == sq(7,7)) ns.castle_rights &= ~0x4;

    /* halfmove clock */
    if (piece == W_PAWN || piece == B_PAWN || captured != EMPTY)
        ns.halfmove = 0;
    else
        ns.halfmove++;

    ns.side = -s->side;
    if (s->side == BLACK) ns.fullmove++;
    return ns;
}

/* ── Move generation ─────────────────────────────────────────────────── */
#define MAX_MOVES 256

static int gen_moves(const State *s, Move *list) {
    int cnt = 0;
    int side = s->side;

#define ADD(f,t,pr,ca,epsq) do { \
    list[cnt].from=(f); list[cnt].to=(t); \
    list[cnt].promo=(pr); list[cnt].castle=(ca); \
    list[cnt].ep=(epsq); cnt++; } while(0)

    for (int from = 0; from < 64; from++) {
        int piece = s->board[from];
        if (!piece || piece_color(piece) != side) continue;

        int r = rank_of(from), f = file_of(from);

        /* ── Pawns ── */
        if (piece == W_PAWN || piece == B_PAWN) {
            int dir = (side == WHITE) ? 1 : -1;
            int start_rank = (side == WHITE) ? 1 : 6;
            int promo_rank = (side == WHITE) ? 7 : 0;
            int nr = r + dir;

            /* single push */
            if (on_board(nr, f) && s->board[sq(nr,f)] == EMPTY) {
                if (nr == promo_rank) {
                    ADD(from, sq(nr,f), W_QUEEN, 0, -1);
                } else {
                    ADD(from, sq(nr,f), 0, 0, -1);
                    /* double push */
                    if (r == start_rank && s->board[sq(nr+dir,f)] == EMPTY)
                        ADD(from, sq(nr+dir,f), 0, 0, -1);
                }
            }
            /* captures */
            for (int df = -1; df <= 1; df += 2) {
                int nf = f + df;
                if (!on_board(nr, nf)) continue;
                int dest = s->board[sq(nr,nf)];
                /* normal capture */
                if (dest != EMPTY && piece_color(dest) == -side) {
                    if (nr == promo_rank) ADD(from, sq(nr,nf), W_QUEEN, 0, -1);
                    else                  ADD(from, sq(nr,nf), 0, 0, -1);
                }
                /* en passant */
                if (sq(nr,nf) == s->ep_sq)
                    ADD(from, sq(nr,nf), 0, 0, sq(r,nf));
            }

        /* ── Knights ── */
        } else if (piece == W_KNIGHT || piece == B_KNIGHT) {
            static const int kd[8][2] = {{2,1},{2,-1},{-2,1},{-2,-1},
                                          {1,2},{1,-2},{-1,2},{-1,-2}};
            for (int i = 0; i < 8; i++) {
                int nr = r+kd[i][0], nf = f+kd[i][1];
                if (!on_board(nr,nf)) continue;
                int dest = s->board[sq(nr,nf)];
                if (dest == EMPTY || piece_color(dest) == -side)
                    ADD(from, sq(nr,nf), 0, 0, -1);
            }

        /* ── Bishops, Rooks, Queens (and King) ── */
        } else {
            int is_bish  = (piece==W_BISHOP||piece==B_BISHOP);
            int is_rook  = (piece==W_ROOK  ||piece==B_ROOK  );
            int is_king  = (piece==W_KING  ||piece==B_KING  );
            (void)(piece==W_QUEEN||piece==B_QUEEN); /* queen uses both axes */

            int dirs[8][2] = {{1,0},{-1,0},{0,1},{0,-1},
                               {1,1},{1,-1},{-1,1},{-1,-1}};
            int d_start = (is_bish) ? 4 : 0;
            int d_end   = (is_rook) ? 4 : 8;

            for (int d = d_start; d < d_end; d++) {
                int nr = r+dirs[d][0], nf = f+dirs[d][1];
                while (on_board(nr,nf)) {
                    int dest = s->board[sq(nr,nf)];
                    if (dest == EMPTY) {
                        ADD(from, sq(nr,nf), 0, 0, -1);
                    } else if (piece_color(dest) == -side) {
                        ADD(from, sq(nr,nf), 0, 0, -1);
                        break;
                    } else break;
                    if (is_king) break;  /* king: one step only */
                    nr += dirs[d][0]; nf += dirs[d][1];
                }
            }

            /* Castling */
            if (is_king) {
                if (side == WHITE) {
                    /* Kingside */
                    if ((s->castle_rights & 0x1) &&
                        s->board[sq(0,5)]==EMPTY && s->board[sq(0,6)]==EMPTY &&
                        !is_attacked(s,sq(0,4),BLACK) &&
                        !is_attacked(s,sq(0,5),BLACK) &&
                        !is_attacked(s,sq(0,6),BLACK))
                        ADD(from, sq(0,6), 0, 1, -1);
                    /* Queenside */
                    if ((s->castle_rights & 0x2) &&
                        s->board[sq(0,3)]==EMPTY && s->board[sq(0,2)]==EMPTY &&
                        s->board[sq(0,1)]==EMPTY &&
                        !is_attacked(s,sq(0,4),BLACK) &&
                        !is_attacked(s,sq(0,3),BLACK) &&
                        !is_attacked(s,sq(0,2),BLACK))
                        ADD(from, sq(0,2), 0, 2, -1);
                } else {
                    if ((s->castle_rights & 0x4) &&
                        s->board[sq(7,5)]==EMPTY && s->board[sq(7,6)]==EMPTY &&
                        !is_attacked(s,sq(7,4),WHITE) &&
                        !is_attacked(s,sq(7,5),WHITE) &&
                        !is_attacked(s,sq(7,6),WHITE))
                        ADD(from, sq(7,6), 0, 1, -1);
                    if ((s->castle_rights & 0x8) &&
                        s->board[sq(7,3)]==EMPTY && s->board[sq(7,2)]==EMPTY &&
                        s->board[sq(7,1)]==EMPTY &&
                        !is_attacked(s,sq(7,4),WHITE) &&
                        !is_attacked(s,sq(7,3),WHITE) &&
                        !is_attacked(s,sq(7,2),WHITE))
                        ADD(from, sq(7,2), 0, 2, -1);
                }
            }
        }
    }
#undef ADD

    /* Filter illegal moves (leave own king in check) */
    int legal = 0;
    for (int i = 0; i < cnt; i++) {
        State ns = apply_move(s, list[i]);
        if (!in_check(&ns, side))
            list[legal++] = list[i];
    }
    return legal;
}

/* ── Piece-square tables for evaluation ──────────────────────────────── */
static const int PST_PAWN[64] = {
     0,  0,  0,  0,  0,  0,  0,  0,
    50, 50, 50, 50, 50, 50, 50, 50,
    10, 10, 20, 30, 30, 20, 10, 10,
     5,  5, 10, 25, 25, 10,  5,  5,
     0,  0,  0, 20, 20,  0,  0,  0,
     5, -5,-10,  0,  0,-10, -5,  5,
     5, 10, 10,-20,-20, 10, 10,  5,
     0,  0,  0,  0,  0,  0,  0,  0
};
static const int PST_KNIGHT[64] = {
   -50,-40,-30,-30,-30,-30,-40,-50,
   -40,-20,  0,  0,  0,  0,-20,-40,
   -30,  0, 10, 15, 15, 10,  0,-30,
   -30,  5, 15, 20, 20, 15,  5,-30,
   -30,  0, 15, 20, 20, 15,  0,-30,
   -30,  5, 10, 15, 15, 10,  5,-30,
   -40,-20,  0,  5,  5,  0,-20,-40,
   -50,-40,-30,-30,-30,-30,-40,-50
};
static const int PST_BISHOP[64] = {
   -20,-10,-10,-10,-10,-10,-10,-20,
   -10,  0,  0,  0,  0,  0,  0,-10,
   -10,  0,  5, 10, 10,  5,  0,-10,
   -10,  5,  5, 10, 10,  5,  5,-10,
   -10,  0, 10, 10, 10, 10,  0,-10,
   -10, 10, 10, 10, 10, 10, 10,-10,
   -10,  5,  0,  0,  0,  0,  5,-10,
   -20,-10,-10,-10,-10,-10,-10,-20
};
static const int PST_ROOK[64] = {
     0,  0,  0,  0,  0,  0,  0,  0,
     5, 10, 10, 10, 10, 10, 10,  5,
    -5,  0,  0,  0,  0,  0,  0, -5,
    -5,  0,  0,  0,  0,  0,  0, -5,
    -5,  0,  0,  0,  0,  0,  0, -5,
    -5,  0,  0,  0,  0,  0,  0, -5,
    -5,  0,  0,  0,  0,  0,  0, -5,
     0,  0,  0,  5,  5,  0,  0,  0
};
static const int PST_QUEEN[64] = {
   -20,-10,-10, -5, -5,-10,-10,-20,
   -10,  0,  0,  0,  0,  0,  0,-10,
   -10,  0,  5,  5,  5,  5,  0,-10,
    -5,  0,  5,  5,  5,  5,  0, -5,
     0,  0,  5,  5,  5,  5,  0, -5,
   -10,  5,  5,  5,  5,  5,  0,-10,
   -10,  0,  5,  0,  0,  0,  0,-10,
   -20,-10,-10, -5, -5,-10,-10,-20
};
static const int PST_KING[64] = {
   -30,-40,-40,-50,-50,-40,-40,-30,
   -30,-40,-40,-50,-50,-40,-40,-30,
   -30,-40,-40,-50,-50,-40,-40,-30,
   -30,-40,-40,-50,-50,-40,-40,-30,
   -20,-30,-30,-40,-40,-30,-30,-20,
   -10,-20,-20,-20,-20,-20,-20,-10,
    20, 20,  0,  0,  0,  0, 20, 20,
    20, 30, 10,  0,  0, 10, 30, 20
};

static const int PIECE_VALUE[7] = {0, 100, 320, 330, 500, 900, 20000};

static int pst_index(int sq_idx, int color) {
    /* White uses rank 0 at bottom; PST is indexed rank 7 at index 0 */
    return (color == WHITE) ? (63 - sq_idx) : sq_idx;
}

static int evaluate(const State *s) {
    int score = 0;
    for (int i = 0; i < 64; i++) {
        int p = s->board[i];
        if (!p) continue;
        int col   = piece_color(p);
        int ap    = (p > 0) ? p : -p;
        int val   = PIECE_VALUE[ap];
        int pi    = pst_index(i, col);
        int bonus = 0;
        switch (ap) {
            case W_PAWN:   bonus = PST_PAWN[pi];   break;
            case W_KNIGHT: bonus = PST_KNIGHT[pi];  break;
            case W_BISHOP: bonus = PST_BISHOP[pi];  break;
            case W_ROOK:   bonus = PST_ROOK[pi];    break;
            case W_QUEEN:  bonus = PST_QUEEN[pi];   break;
            case W_KING:   bonus = PST_KING[pi];    break;
        }
        score += col * (val + bonus);
    }
    return score;
}

/* ── Alpha-beta minimax ───────────────────────────────────────────────── */
static Move best_move_found;

static int alphabeta(State *s, int depth, int alpha, int beta, int maximising) {
    Move moves[MAX_MOVES];
    int  cnt = gen_moves(s, moves);

    if (cnt == 0) {
        if (in_check(s, s->side))
            return maximising ? -100000 : 100000;  /* checkmate */
        return 0;  /* stalemate */
    }
    if (depth == 0) return evaluate(s);

    if (maximising) {
        int best = INT_MIN;
        for (int i = 0; i < cnt; i++) {
            State ns = apply_move(s, moves[i]);
            int v = alphabeta(&ns, depth-1, alpha, beta, 0);
            if (v > best) { best = v; }
            if (v > alpha) alpha = v;
            if (alpha >= beta) break;
        }
        return best;
    } else {
        int best = INT_MAX;
        for (int i = 0; i < cnt; i++) {
            State ns = apply_move(s, moves[i]);
            int v = alphabeta(&ns, depth-1, alpha, beta, 1);
            if (v < best) { best = v; }
            if (v < beta) beta = v;
            if (alpha >= beta) break;
        }
        return best;
    }
}

static int engine_move(State *s, int depth) {
    Move moves[MAX_MOVES];
    int  cnt = gen_moves(s, moves);
    if (cnt == 0) return 0;

    int   best_val = INT_MAX;
    Move  best     = moves[0];

    for (int i = 0; i < cnt; i++) {
        State ns = apply_move(s, moves[i]);
        int v = alphabeta(&ns, depth-1, INT_MIN, INT_MAX, 1);
        if (v < best_val) { best_val = v; best = moves[i]; }
    }
    best_move_found = best;
    return 1;
}

/* ── Board display ────────────────────────────────────────────────────── */
static void print_captured(void) {
    printf("  " BOLD "White captured: " RST);
    for (int i = 0; i < bc_count; i++) {
        int p = (b_captured[i] < 0) ? -b_captured[i] : b_captured[i];
        printf("%s%s" RST, FG_WHITE, piece_glyph(-p));
    }
    printf("\n");
    printf("  " BOLD "Black captured: " RST);
    for (int i = 0; i < wc_count; i++) {
        int p = (w_captured[i] < 0) ? -w_captured[i] : w_captured[i];
        printf("%s%s" RST, FG_BLACK, piece_glyph(p));
    }
    printf("\n");
}

static void print_history(void) {
    printf("  " BOLD "Moves: " RST);
    for (int i = 0; i < hist_count && i < 20; i++) {
        if (i % 2 == 0) printf(CYAN "%d." RST, i/2+1);
        printf(" %s ", move_history[i]);
    }
    if (hist_count > 20) printf(DIM "..." RST);
    printf("\n");
}

static void draw_board(const State *s) {
    printf("\n");
    printf("      a   b   c   d   e   f   g   h\n");
    printf("    ┌───┬───┬───┬───┬───┬───┬───┬───┐\n");

    for (int r = 7; r >= 0; r--) {
        printf("  %d │", r + 1);
        for (int f = 0; f < 8; f++) {
            int  sq_idx = sq(r, f);
            int  piece  = s->board[sq_idx];
            int  light  = (r + f) % 2 == 1;
            int  hilit  = (sq_idx == last_from || sq_idx == last_to);

            const char *bg = hilit  ? BG_HIGH  :
                             light  ? BG_LIGHT : BG_DARK;

            const char *fg = (piece_color(piece) == WHITE) ? FG_WHITE :
                             (piece_color(piece) == BLACK) ? FG_BLACK : "";

            printf("%s%s %s %s│" RST,
                   bg, fg,
                   piece_glyph(piece),
                   RST);
        }
        printf(" %d\n", r + 1);

        if (r > 0)
            printf("    ├───┼───┼───┼───┼───┼───┼───┼───┤\n");
    }

    printf("    └───┴───┴───┴───┴───┴───┴───┴───┘\n");
    printf("      a   b   c   d   e   f   g   h\n\n");

    print_captured();
    printf("\n");
    print_history();
    printf("\n");
}

/* ── Parse move string ────────────────────────────────────────────────── */
static int parse_move(const char *s_in, const State *gs, Move *out) {
    char s[16];
    strncpy(s, s_in, sizeof s - 1);
    s[sizeof s - 1] = '\0';

    /* strip optional dash: e2-e4 → e2e4 */
    char clean[8]; int ci = 0;
    for (int i = 0; s[i] && ci < 7; i++)
        if (s[i] != '-') clean[ci++] = (char)tolower((unsigned char)s[i]);
    clean[ci] = '\0';

    if (strlen(clean) < 4) return 0;
    int fc = clean[0] - 'a';
    int fr = clean[1] - '1';
    int tc = clean[2] - 'a';
    int tr = clean[3] - '1';

    if (!on_board(fr,fc) || !on_board(tr,tc)) return 0;

    /* find this move in legal move list */
    Move moves[MAX_MOVES];
    int cnt = gen_moves(gs, moves);

    int from = sq(fr,fc), to = sq(tr,tc);
    for (int i = 0; i < cnt; i++) {
        if (moves[i].from == from && moves[i].to == to) {
            *out = moves[i];
            return 1;
        }
    }
    return 0;
}

/* ── Format move for history ──────────────────────────────────────────── */
static void format_move(const State *s, Move m, char *buf) {
    int piece = s->board[m.from];
    char pfx[4] = "";
    int ap = (piece > 0) ? piece : -piece;
    switch (ap) {
        case W_KNIGHT: strcpy(pfx, "N"); break;
        case W_BISHOP: strcpy(pfx, "B"); break;
        case W_ROOK:   strcpy(pfx, "R"); break;
        case W_QUEEN:  strcpy(pfx, "Q"); break;
        case W_KING:
            if (m.castle == 1) { strcpy(buf, "O-O");   return; }
            if (m.castle == 2) { strcpy(buf, "O-O-O"); return; }
            strcpy(pfx, "K"); break;
    }
    int cap = (s->board[m.to] != EMPTY || m.ep >= 0);
    sprintf(buf, "%s%c%c%s%c%c",
            pfx,
            'a' + file_of(m.from), '1' + rank_of(m.from),
            cap ? "x" : "-",
            'a' + file_of(m.to),   '1' + rank_of(m.to));
}

/* ── Banner ─────────────────────────────────────────────────────────────── */
static void banner(void) {
    printf(CYAN BOLD
        "╔══════════════════════════════════════════════════╗\n"
        "║          Terminal Chess Engine  v1.0            ║\n"
        "║    You play White  ·  Engine plays Black        ║\n"
        "╚══════════════════════════════════════════════════╝\n"
        RST);
    printf(DIM
        "  Input format:  e2e4  or  e2-e4\n"
        "  Commands:      resign | quit\n\n"
        RST);
}

/* ── Main ───────────────────────────────────────────────────────────────── */
int main(void) {
    banner();

    State game;
    init_state(&game);
    draw_board(&game);

    char line[64];
    while (1) {
        /* ── Status line ── */
        int check = in_check(&game, game.side);
        Move legal[MAX_MOVES];
        int  legal_cnt = gen_moves(&game, legal);

        if (legal_cnt == 0) {
            if (check) {
                const char *winner = (game.side == WHITE) ? "Black" : "White";
                printf(RED BOLD "  Checkmate! %s wins.\n" RST, winner);
            } else {
                printf(YELLOW BOLD "  Stalemate — draw.\n" RST);
            }
            break;
        }

        if (check)
            printf(RED BOLD "  CHECK!\n" RST);

        if (game.side == WHITE) {
            printf(GREEN "chess [White]> " RST);
            fflush(stdout);

            if (!fgets(line, sizeof line, stdin)) break;
            /* trim */
            int n = (int)strlen(line);
            while (n > 0 && isspace((unsigned char)line[n-1])) line[--n] = '\0';

            if (!*line) continue;
            if (strcmp(line, "quit") == 0 || strcmp(line, "resign") == 0) {
                printf(DIM "  You resigned. Goodbye!\n" RST);
                break;
            }

            Move m;
            if (!parse_move(line, &game, &m)) {
                printf(RED "  Illegal move. Format: e2e4\n" RST);
                continue;
            }

            /* record */
            int cap = (game.board[m.to] != EMPTY);
            if (cap && wc_count < 16)
                w_captured[wc_count++] = game.board[m.to];
            if (hist_count < MAX_HIST) {
                format_move(&game, m, move_history[hist_count]);
                hist_count++;
            }
            last_from = m.from;
            last_to   = m.to;

            game = apply_move(&game, m);
            draw_board(&game);

        } else {
            /* Engine turn */
            printf(MAGENTA BOLD "  Engine thinking...\n" RST);
            fflush(stdout);

            if (!engine_move(&game, 4)) {
                printf(RED "  Engine has no moves (bug?)\n" RST);
                break;
            }
            Move em = best_move_found;

            int cap = (game.board[em.to] != EMPTY);
            if (cap && bc_count < 16)
                b_captured[bc_count++] = game.board[em.to];

            /* print engine move */
            char mstr[16];
            format_move(&game, em, mstr);
            printf(MAGENTA "  Engine plays: %s\n" RST, mstr);

            if (hist_count < MAX_HIST) {
                strcpy(move_history[hist_count], mstr);
                hist_count++;
            }
            last_from = em.from;
            last_to   = em.to;

            game = apply_move(&game, em);
            draw_board(&game);
        }
    }

    return 0;
}
