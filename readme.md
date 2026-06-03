# Chess Engine in C

A fully legal **terminal chess engine** with a depth-4 **minimax AI** using alpha-beta pruning, rendered on an ANSI-coloured board with Unicode chess pieces.

Written in pure C with no external dependencies. Part of the Corg-Labs collection.

---

# Features
- Full legal move generation for all six piece types
- Castling (kingside and queenside) and en passant
- Pawn promotion (auto-promotes to queen)
- Depth-4 alpha-beta pruning minimax engine (plays Black)
- Piece-square tables for positional evaluation
- Unicode glyphs: ♙♘♗♖♕♔ / ♟♞♝♜♛♚
- ANSI colour board with last-move highlighting
- Captured pieces sidebar and scrolling move history
- Check, checkmate, and stalemate detection

---

# Tutorial

## 1. Board Representation

The board is a flat `int[64]` array in a `State` struct. Positive values are white pieces; negative values are black pieces; zero is empty. Pieces are numbered 1–6 (Pawn through King).

```c
#define W_PAWN  1
#define W_KNIGHT 2
#define W_BISHOP 3
#define W_ROOK  4
#define W_QUEEN 5
#define W_KING  6
/* Black mirrors: B_PAWN = -1, B_KNIGHT = -2, ... */

typedef struct {
    int  board[64];
    int  side;          /* WHITE (1) or BLACK (-1) to move */
    int  ep_sq;         /* en-passant target square, -1 if none */
    int  castle_rights; /* bitfield: bit0=WK, bit1=WQ, bit2=BK, bit3=BQ */
    int  halfmove;
    int  fullmove;
} State;
```

Square indexing is `rank * 8 + file`, with rank 0 at White's back row.

```c
static inline int sq(int r, int f) { return r * 8 + f; }
static inline int rank_of(int sq)  { return sq >> 3; }
static inline int file_of(int sq)  { return sq & 7; }
```

## 2. Move Struct

A `Move` captures everything needed to apply or un-apply a move without storing the full board history.

```c
typedef struct {
    int from, to;   /* 0-63 indices */
    int promo;      /* promotion piece, 0 = none */
    int castle;     /* 0=none, 1=kingside, 2=queenside */
    int ep;         /* en-passant capture square, -1 = none */
} Move;
```

## 3. Move Generation

`gen_moves` iterates over all squares, generates pseudo-legal moves for each friendly piece, then filters for legality by checking whether the side's king is in check after each move.

```c
static int gen_moves(const State *s, Move *list) {
    int cnt = 0;
    /* ... generate pawns, knights, sliding pieces, castling ... */

    /* Filter: only keep moves that don't leave own king in check */
    int legal = 0;
    for (int i = 0; i < cnt; i++) {
        State ns = apply_move(s, list[i]);
        if (!in_check(&ns, s->side))
            list[legal++] = list[i];
    }
    return legal;
}
```

Sliding pieces (bishop, rook, queen) use direction vectors and loop until hitting a piece or the board edge:

```c
int dirs[8][2] = {{1,0},{-1,0},{0,1},{0,-1},
                   {1,1},{1,-1},{-1,1},{-1,-1}};
int d_start = is_bish ? 4 : 0;   /* bishops: diagonals only */
int d_end   = is_rook ? 4 : 8;   /* rooks: straights only   */

for (int d = d_start; d < d_end; d++) {
    int nr = r + dirs[d][0], nf = f + dirs[d][1];
    while (on_board(nr, nf)) {
        int dest = s->board[sq(nr, nf)];
        if (dest == EMPTY) { ADD(from, sq(nr,nf), 0, 0, -1); }
        else if (piece_color(dest) == -side) { ADD(from, sq(nr,nf), 0, 0, -1); break; }
        else break;
        if (is_king) break;   /* king moves only one square */
        nr += dirs[d][0]; nf += dirs[d][1];
    }
}
```

## 4. Castling Logic

Castling is generated as a special king move. Three conditions must all hold: the castling right bit is set, the squares between king and rook are empty, and none of the king-traversal squares are attacked.

```c
/* White kingside */
if ((s->castle_rights & 0x1) &&
    s->board[sq(0,5)] == EMPTY &&
    s->board[sq(0,6)] == EMPTY &&
    !is_attacked(s, sq(0,4), BLACK) &&
    !is_attacked(s, sq(0,5), BLACK) &&
    !is_attacked(s, sq(0,6), BLACK))
    ADD(from, sq(0,6), 0, 1, -1);   /* castle flag = 1 */
```

When `apply_move` sees `castle == 1`, it also teleports the rook from h1 to f1.

## 5. Static Evaluation

The evaluator sums material value plus a positional bonus from piece-square tables for every piece on the board.

```c
static const int PIECE_VALUE[7] = {0, 100, 320, 330, 500, 900, 20000};

static int evaluate(const State *s) {
    int score = 0;
    for (int i = 0; i < 64; i++) {
        int p   = s->board[i];
        if (!p) continue;
        int col = piece_color(p);
        int ap  = (p > 0) ? p : -p;
        int pi  = pst_index(i, col);   /* flip rank for black */
        score  += col * (PIECE_VALUE[ap] + pst_bonus(ap, pi));
    }
    return score;   /* positive = White advantage */
}
```

Piece-square tables are 64-element `int` arrays that reward good squares (e.g., pawns near promotion, knights in the centre).

## 6. Minimax with Alpha-Beta Pruning

Alpha-beta is a minimax improvement that prunes branches guaranteed not to affect the result. `alpha` is White's best guaranteed score; `beta` is Black's best guaranteed score. When `alpha >= beta`, the remaining siblings cannot improve the result and are skipped.

```c
static int alphabeta(State *s, int depth, int alpha, int beta, int maximising) {
    Move moves[MAX_MOVES];
    int  cnt = gen_moves(s, moves);

    if (cnt == 0)   return in_check(s, s->side) ? (maximising ? -100000 : 100000) : 0;
    if (depth == 0) return evaluate(s);

    if (maximising) {
        int best = INT_MIN;
        for (int i = 0; i < cnt; i++) {
            State ns = apply_move(s, moves[i]);
            int v = alphabeta(&ns, depth - 1, alpha, beta, 0);
            if (v > best)  best  = v;
            if (v > alpha) alpha = v;
            if (alpha >= beta) break;   /* beta cut-off */
        }
        return best;
    } else {
        int best = INT_MAX;
        for (int i = 0; i < cnt; i++) {
            State ns = apply_move(s, moves[i]);
            int v = alphabeta(&ns, depth - 1, alpha, beta, 1);
            if (v < best) best = v;
            if (v < beta) beta = v;
            if (alpha >= beta) break;   /* alpha cut-off */
        }
        return best;
    }
}
```

The engine entry point calls `alphabeta` on each root move and picks the one with the lowest score (Black minimises).

## 7. Board Rendering

The board is printed rank by rank from rank 8 down to rank 1. Each square gets a background colour (light or dark) and an optional bright-green highlight for the last move's source and destination squares.

```c
for (int r = 7; r >= 0; r--) {
    printf("  %d │", r + 1);
    for (int f = 0; f < 8; f++) {
        int sq_idx = sq(r, f);
        int piece  = s->board[sq_idx];
        int light  = (r + f) % 2 == 1;
        int hilit  = (sq_idx == last_from || sq_idx == last_to);

        const char *bg = hilit ? BG_HIGH : light ? BG_LIGHT : BG_DARK;
        const char *fg = (piece_color(piece) == WHITE) ? FG_WHITE :
                         (piece_color(piece) == BLACK) ? FG_BLACK : "";

        printf("%s%s %s %s│" RST, bg, fg, piece_glyph(piece), RST);
    }
    printf(" %d\n", r + 1);
}
```

`piece_glyph` maps piece integers to their Unicode codepoints from a 13-element string array.

---

# Build
```
gcc chess.c -o chess
```

# Run
```
./chess
```

# Controls

| Input | Action |
|---|---|
| `e2e4` or `e2-e4` | Move piece from e2 to e4 |
| `resign` | Concede the game |
| `quit` | Exit immediately |

You play White. The engine responds as Black after every move.

---

# Concepts Practiced
- 8x8 signed-integer board encoding (positive=white, negative=black)
- Pseudo-legal move generation filtered by legality check
- Castling rights bitfield and en-passant target square tracking
- Apply-move by value (copy `State` struct, no undo needed)
- Minimax adversarial search with alpha-beta pruning
- Piece-square tables for positional evaluation
- Attack map generation for check/checkmate/stalemate detection
- Unicode and ANSI escape rendering in a terminal grid

---

# Dependencies
Standard C libraries only: `stdio.h`, `stdlib.h`, `string.h`, `ctype.h`, `limits.h`
