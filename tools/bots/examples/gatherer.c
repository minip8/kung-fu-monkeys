/* gatherer: the C SDK's example bot for the harvest kind (bot ABI v2,
 * architecture v0.82) and the kind's smoke test: it carries bananas home and
 * little else (no dash, no catch). Each tick:
 *   1. holding a banana in its own home zone (zone `self.team`), or once set
 *      on going home, it walks home and stands there until its hand is empty
 *      (the kind banks one a beat, modeData word 0);
 *   2. with room in its hand, it walks to the nearest banana pickup it sees
 *      (straight-line nearest; it keeps its choice while the banana lies);
 *   3. else, holding something (a full hand, or nothing to fetch), it sets
 *      off home; empty-handed with nothing to fetch, it walks to the tree it
 *      works and waits there for the fruit;
 *   4. whatever it does, holding two bananas or more it throws at a visible
 *      enemy in range with no wall cell in between (the banana comes back).
 * The tree it works (an arena.spawners entry with `tree` 1): the team's
 * monkeys share the trees out by their rank among the team's slots, no
 * call needed, each tree weighted by what a trip to it costs, its beat plus
 * twice its walking distance from the home (gatherer_assign), so the rich
 * and near trees get more monkeys.
 * Paths go round walls and pits over a cell map built from arena.walls and
 * arena.hazards: a cell is open when the monkey's circle at its centre lies
 * in the outer box and overlaps no wall, and its centre is on no pit; a
 * diagonal step needs both orthogonal neighbours open (the grid's rule). The
 * cell size is learnt from the own cell at rest in a grid mode (1 unit until
 * then, and in a continuous mode). A goal (the home, the tree it works, the
 * banana fetched) is a breadth-first distance field from the goal's cells,
 * grown at most GATHERER_GROW cells a tick and walked downhill; until it
 * reaches the monkey's cell it walks straight at the goal. The map and the
 * home field are built over ticks of their own, in the round's countdown. So
 * no tick does more than a bounded amount of work. Build:
 * tools/bots/build_example.sh gatherer. */
#define KFM_BOT_MAIN
#include "../sdk-c/kfm_bot.h"

#define GATHERER_MAX_CELLS 3600u /* the padded map: (cols + 2) x (rows + 2) */
#define GATHERER_GROW 96u        /* cells a field grows by in a tick */
#define GATHERER_STUCK_TICKS 12u /* on a banana's cell this long: give up on it */
#define GATHERER_GIVE_UP_TICKS 600u
#define GATHERER_PREP_GROW 128u /* cells the home field grows by in a tick of its own */
#define GATHERER_FAR 0xFFFFu

enum { GOAL_HOME = 0, GOAL_TREE = 1, GOAL_BANANA = 2 };

/* The eight octants (0 east, counter-clockwise; rows grow southward). */
static const int8_t gatherer_dc[8] = {1, 1, 0, -1, -1, -1, 0, 1};
static const int8_t gatherer_dr[8] = {0, -1, -1, -1, 0, 1, 1, 1};

/* The map: cell (col, row) is index (row + 1) * stride + col + 1, inside a
 * closed border, so a neighbour index is never out of range. */
static uint8_t g_open[GATHERER_MAX_CELLS];
static uint8_t g_moves[GATHERER_MAX_CELLS]; /* bit d: the step in octant d is open */
static uint8_t g_solid[GATHERER_MAX_CELLS]; /* 1: the cell's centre is in a wall (blocks a throw) */
static int32_t g_stride, g_cols, g_rows;
static int32_t g_step[8]; /* index deltas per octant */
static kfm_fx g_cell;     /* the cell size in use, Q16.16 */
static int g_built; /* the map for g_cell: 0 to build, 1 the cells, 2 and 3 the steps (two
                       halves), 4 the walls, 5 the home field seeded, 6 done */
#define GATHERER_READY 6
static int g_ok;          /* the map fits (else: straight lines) */

/* A breadth-first field: the distance in steps to its sources. */
typedef struct gatherer_field {
  uint16_t dist[GATHERER_MAX_CELLS];
  uint16_t queue[GATHERER_MAX_CELLS];
  uint32_t head, tail;
  int started; /* sources seeded */
  int32_t source; /* GOAL_BANANA, GOAL_TREE: the goal's cell */
} gatherer_field;
static gatherer_field g_fields[3];

static int g_banking;          /* set on going home: until the hand is empty */
static int g_target;           /* the pickup slot fetched, -1 none */
static int g_tree;             /* the spawner it works, -1 none */
static kfm_fx g_banana_speed;  /* the own banana's flight speed, last seen */
static uint32_t g_target_spot; /* its position folded (a reused slot is new) */
static uint32_t g_stuck_ticks; /* ticks on the cell of the banana it fetches */
static uint32_t g_ignore_until[KFM_MAX_PICKUPS];
static uint32_t g_ignore_spot[KFM_MAX_PICKUPS];

/* A pickup's position folded into a word. */
static uint32_t gatherer_spot(const kfm_obs_pickup* p) {
  return (uint32_t)p->posX ^ ((uint32_t)p->posY * 2654435761u);
}

static int gatherer_in_box(kfm_fx x, kfm_fx y, kfm_fx minX, kfm_fx minY, kfm_fx maxX, kfm_fx maxY) {
  return x >= minX && x <= maxX && y >= minY && y <= maxY;
}

/* The closed box and the circle overlap (the sim's test: closer than r). */
static int gatherer_circle_hits(const kfm_obs_box* b, kfm_fx x, kfm_fx y, kfm_fx r) {
  int64_t dx = (int64_t)x - kfm_fx_clamp(x, b->minX, b->maxX);
  int64_t dy = (int64_t)y - kfm_fx_clamp(y, b->minY, b->maxY);
  return (uint64_t)((dx * dx) + (dy * dy)) < kfm_fx_sq(r);
}

static kfm_fx gatherer_centre_x(int32_t col) {
  return (kfm_fx)(((int64_t)col * g_cell) - kfm_obs.arena.halfExtents.x + (g_cell / 2));
}
static kfm_fx gatherer_centre_y(int32_t row) {
  return (kfm_fx)((int64_t)kfm_obs.arena.halfExtents.y - ((int64_t)row * g_cell) - (g_cell / 2));
}
/* The column (row) of a coordinate, clamped to the last; -1 before the first. */
static int32_t gatherer_col_of(kfm_fx x) {
  int64_t off = (int64_t)x + kfm_obs.arena.halfExtents.x;
  int64_t c = off / g_cell;
  if (off < 0) return -1;
  return c >= g_cols ? g_cols - 1 : (int32_t)c;
}
static int32_t gatherer_row_of(kfm_fx y) {
  int64_t off = (int64_t)kfm_obs.arena.halfExtents.y - y;
  int64_t r = off / g_cell;
  if (off < 0) return -1;
  return r >= g_rows ? g_rows - 1 : (int32_t)r;
}
/* The map index of the cell holding a point, -1 off the map. */
static int32_t gatherer_index_of(kfm_fx x, kfm_fx y) {
  int32_t col = gatherer_col_of(x);
  int32_t row = gatherer_row_of(y);
  if (col < 0 || row < 0) return -1;
  return ((row + 1) * g_stride) + col + 1;
}
static kfm_vec gatherer_centre_of(int32_t i) {
  return kfm_v(gatherer_centre_x((i % g_stride) - 1), gatherer_centre_y((i / g_stride) - 1));
}

/* Sets map[k] = v on the cells a box covers: the circle of radius r at a
 * cell's centre overlaps it (r > 0), or the centre lies in it (r == 0). */
static void gatherer_mark_box(const kfm_obs_box* b, kfm_fx r, uint8_t* map, uint8_t v) {
  int32_t c0 = gatherer_col_of(b->minX - r);
  int32_t c1 = gatherer_col_of(b->maxX + r);
  int32_t r0 = gatherer_row_of(b->maxY + r);
  int32_t r1 = gatherer_row_of(b->minY - r);
  int32_t col;
  int32_t row;
  if (c0 < 0) c0 = 0;
  if (r0 < 0) r0 = 0;
  if (r > 0) {
    /* A centre r or more off the box on an axis cannot touch it: on a map
     * of cell-aligned walls and r = cell / 2 only the wall's own cells. */
    if (c0 <= c1 && gatherer_centre_x(c0) <= b->minX - r) ++c0;
    if (c1 >= c0 && gatherer_centre_x(c1) >= b->maxX + r) --c1;
    if (r0 <= r1 && gatherer_centre_y(r0) >= b->maxY + r) ++r0;
    if (r1 >= r0 && gatherer_centre_y(r1) <= b->minY - r) --r1;
  }
  for (row = r0; row <= r1; ++row) {
    kfm_fx y = gatherer_centre_y(row);
    for (col = c0; col <= c1; ++col) {
      kfm_fx x = gatherer_centre_x(col);
      int hit = r > 0 ? gatherer_circle_hits(b, x, y, r)
                      : gatherer_in_box(x, y, b->minX, b->minY, b->maxX, b->maxY);
      if (hit) map[((row + 1) * g_stride) + col + 1] = v;
    }
  }
}

static void gatherer_build(const kfm_observation* o) {
  const kfm_obs_arena* a = &o->arena;
  kfm_fx r = o->self.radius;
  int32_t i;
  int32_t col;
  int32_t row;
  g_built = 1;
  g_ok = 0;
  g_tree = -1;
  g_target = -1;
  for (i = 0; i < 3; ++i)
    g_fields[i].started = 0;
  if (g_cell <= 0 || a->halfExtents.x <= 0 || a->halfExtents.y <= 0) return;
  g_cols = (int32_t)(((int64_t)a->halfExtents.x * 2) / g_cell);
  g_rows = (int32_t)(((int64_t)a->halfExtents.y * 2) / g_cell);
  if (g_cols <= 0 || g_rows <= 0 ||
      ((int64_t)g_cols + 2) * ((int64_t)g_rows + 2) > (int64_t)GATHERER_MAX_CELLS) {
    return;
  }
  g_stride = g_cols + 2;
  for (i = 0; i < 8; ++i)
    g_step[i] = (gatherer_dr[i] * g_stride) + gatherer_dc[i];
  for (i = 0; i < g_stride * (g_rows + 2); ++i) {
    g_open[i] = 0;
    g_solid[i] = 0;
    g_moves[i] = 0;
  }
  for (row = 0; row < g_rows; ++row) {
    int y_in = kfm_fx_abs(gatherer_centre_y(row)) <= a->halfExtents.y - r;
    uint8_t* line = &g_open[((row + 1) * g_stride) + 1];
    kfm_fx x = gatherer_centre_x(0);
    for (col = 0; col < g_cols; ++col, x += g_cell)
      line[col] = (uint8_t)(y_in && kfm_fx_abs(x) <= a->halfExtents.x - r);
  }
  for (i = 0; i < a->wallCount && i < KFM_MAX_WALLS; ++i)
    gatherer_mark_box(&a->walls[i], r, g_open, 0);
  for (i = 0; i < a->hazardCount && i < KFM_MAX_HAZARDS; ++i) {
    if (a->hazards[i].kind == KFM_HAZARD_PIT) gatherer_mark_box(&a->hazards[i].box, 0, g_open, 0);
  }
  g_ok = 1;
}

/* The open steps of every cell of rows [from, to) (a closed cell's too: the
 * monkey may stand on one in a continuous mode, pushed against a wall); the
 * border's are 0 (gatherer_build). */
static void gatherer_build_steps(int32_t from, int32_t to) {
  const int32_t st = g_stride;
  int32_t col;
  int32_t row;
  for (row = from + 1; row <= to; ++row) {
    const uint8_t* k = &g_open[(row * st) + 1];
    uint8_t* out = &g_moves[(row * st) + 1];
    for (col = 0; col < g_cols; ++col, ++k) {
      uint32_t e = k[1];
      uint32_t n = k[-st];
      uint32_t w = k[-1];
      uint32_t s = k[st];
      out[col] = (uint8_t)(e | ((k[1 - st] & e & n) << 1) | (n << 2) | ((k[-1 - st] & n & w) << 3) |
                           (w << 4) | ((k[st - 1] & w & s) << 5) | (s << 6) |
                           ((k[st + 1] & s & e) << 7));
    }
  }
}

/* The wall cells (a centre in a wall box), which block a throw's line. */
static void gatherer_build_walls(const kfm_observation* o) {
  int32_t i;
  for (i = 0; i < o->arena.wallCount && i < KFM_MAX_WALLS; ++i)
    gatherer_mark_box(&o->arena.walls[i], 0, g_solid, 1);
}

static void gatherer_add_source(gatherer_field* f, int32_t k) {
  if (k < 0 || g_open[k] == 0 || f->dist[k] == 0) return;
  f->dist[k] = 0;
  f->queue[f->tail++] = (uint16_t)k;
}

/* Seeds a field with its goal's open cells: the home zone's, or the one of
 * the tree it works or the banana it fetches. */
static void gatherer_seed(gatherer_field* f, int goal, const kfm_observation* o) {
  int32_t i;
  int32_t col;
  int32_t row;
  f->started = 1;
  f->head = 0;
  f->tail = 0;
  for (i = 0; i < g_stride * (g_rows + 2); ++i)
    f->dist[i] = GATHERER_FAR;
  if (goal != GOAL_HOME) {
    gatherer_add_source(f, f->source);
  } else if (o->self.team < o->zones.count && o->self.team < KFM_MAX_ZONES) {
    const kfm_obs_zone* z = &o->zones.zone[o->self.team];
    for (row = 0; row < g_rows; ++row) {
      kfm_fx y = gatherer_centre_y(row);
      if (y < z->minY || y > z->maxY) continue;
      for (col = 0; col < g_cols; ++col) {
        kfm_fx x = gatherer_centre_x(col);
        if (x >= z->minX && x <= z->maxX) gatherer_add_source(f, ((row + 1) * g_stride) + col + 1);
      }
    }
  }
}

/* Grows a field by at most `budget` cells. */
static void gatherer_grow(gatherer_field* f, uint32_t budget) {
  while (f->head < f->tail && budget > 0) {
    int32_t at = f->queue[f->head++];
    uint16_t next = (uint16_t)(f->dist[at] + 1);
    uint32_t m = g_moves[at];
    --budget;
    while (m != 0) {
      int32_t n = at + g_step[__builtin_ctz(m)];
      m &= m - 1;
      if (f->dist[n] != GATHERER_FAR) continue;
      f->dist[n] = next;
      f->queue[f->tail++] = (uint16_t)n;
    }
  }
}

/* The octant down a field from cell i: the open step to the smallest
 * distance below i's own; -1 at a source, -2 when no neighbour is reached
 * (yet). */
static int gatherer_downhill(const gatherer_field* f, int32_t i) {
  uint16_t best = f->dist[i];
  uint32_t m = g_moves[i];
  int pick = -2;
  if (best == 0) return -1;
  while (m != 0) {
    int d = __builtin_ctz(m);
    m &= m - 1;
    if (f->dist[i + g_step[d]] < best) {
      best = f->dist[i + g_step[d]];
      pick = d;
    }
  }
  return pick;
}

/* The cell size from the own cell while at rest on its centre in a grid
 * mode: x + hx = (col + 1/2) cell, the same down the rows. */
static void gatherer_learn_cell(const kfm_observation* o) {
  const kfm_obs_self* me = &o->self;
  int64_t cx;
  int64_t cy;
  if (o->match.turnTicks == 0 || me->velX != 0 || me->velY != 0 || me->posX != me->prevPosX ||
      me->posY != me->prevPosY || me->cellX < 0 || me->cellY < 0) {
    return;
  }
  cx = (((int64_t)me->posX + o->arena.halfExtents.x) * 2) / ((2 * (int64_t)me->cellX) + 1);
  cy = (((int64_t)o->arena.halfExtents.y - me->posY) * 2) / ((2 * (int64_t)me->cellY) + 1);
  if (cx <= 0 || cx > (int64_t)64 * KFM_FX_ONE || cx - cy > 2 || cy - cx > 2) return;
  if (cx - g_cell > 2 || g_cell - cx > 2) {
    g_cell = (kfm_fx)cx;
    g_built = 0;
  }
}

void kfm_init(uint64_t seed, uint32_t slot) {
  kfm_text t = {{0}, 0};
  int i;
  (void)seed;
  g_cell = KFM_FX_ONE;
  g_built = 0;
  g_ok = 0;
  g_banking = 0;
  g_target = -1;
  g_target_spot = 0;
  g_tree = -1;
  g_banana_speed = 0;
  g_stuck_ticks = 0;
  for (i = 0; i < KFM_MAX_PICKUPS; ++i) {
    g_ignore_until[i] = 0;
    g_ignore_spot[i] = 0;
  }
  kfm_text_str(&t, "gatherer: slot ");
  kfm_text_int(&t, (int64_t)slot);
  kfm_text_str(&t, " ready\n");
  kfm_text_log(&t);
}

static uint64_t gatherer_stand(void) { return kfm_pack_input(0, 0, kfm_obs.self.facing, 0, 0); }

/* The input walking toward a point (facing the way it walks). */
static uint64_t gatherer_toward(kfm_vec me, kfm_vec at) {
  uint8_t dir;
  if (me.x == at.x && me.y == at.y) return gatherer_stand();
  dir = kfm_dir_to(me, at);
  return kfm_pack_input(dir, 255, dir, 0, 0);
}

/* The input taking the step from cell `from` in octant `d`: the octant's own
 * direction in a grid mode, toward the next cell's centre in a continuous one. */
static uint64_t gatherer_step(const kfm_observation* o, kfm_vec me, int32_t from, int d) {
  uint8_t dir = (uint8_t)(d * 32);
  if (o->match.turnTicks > 0) return kfm_pack_input(dir, 255, dir, 0, 0);
  return gatherer_toward(me, gatherer_centre_of(from + g_step[d]));
}

/* Where a goal lies, for the straight walk before its field reaches the
 * monkey: the home zone's centre, the tree it works, the banana. */
static kfm_vec gatherer_goal_point(const kfm_observation* o, int goal, kfm_vec me) {
  if (goal == GOAL_TREE) {
    if (g_tree < 0) return me;
    return kfm_v(o->arena.spawners[g_tree].pos.x, o->arena.spawners[g_tree].pos.y);
  }
  if (goal == GOAL_BANANA) {
    if (g_target < 0) return me;
    return kfm_v(o->pickups[g_target].posX, o->pickups[g_target].posY);
  }
  if (o->self.team >= o->zones.count || o->self.team >= KFM_MAX_ZONES) return me;
  return kfm_v(
      (kfm_fx)(((int64_t)o->zones.zone[o->self.team].minX + o->zones.zone[o->self.team].maxX) / 2),
      (kfm_fx)(((int64_t)o->zones.zone[o->self.team].minY + o->zones.zone[o->self.team].maxY) / 2));
}

/* The tree this monkey works, or -1 when the arena has none it can reach.
 * Every monkey of the team computes the same share-out from the statics and
 * the roster: the r-th monkey (r its rank among the team's present slots)
 * takes the r-th pick of a highest-averages share-out (D'Hondt) over the
 * trees, a tree's weight the inverse of a trip's cost, its beat plus twice
 * its walking distance from the home (steps of the home field once grown,
 * else Chebyshev cells from the home's centre). Ties: the lower spawner. */
static int gatherer_assign(const kfm_observation* o, kfm_vec me) {
  const gatherer_field* home = &g_fields[GOAL_HOME];
  int ready = g_ok && g_built >= GATHERER_READY && home->started;
  kfm_vec hp = gatherer_goal_point(o, GOAL_HOME, me);
  uint32_t cost[KFM_MAX_SPAWNERS];
  uint32_t taken[KFM_MAX_SPAWNERS];
  int rank = 0;
  int i;
  int m;
  for (i = 0; i < KFM_MAX_PLAYERS && i < (int)o->self.slot; ++i) {
    if (o->players[i].present != 0 && o->players[i].team == o->self.team) ++rank;
  }
  for (i = 0; i < KFM_MAX_SPAWNERS; ++i) {
    const kfm_obs_spawner* s = &o->arena.spawners[i];
    uint32_t d;
    cost[i] = 0; /* not a candidate */
    taken[i] = 0;
    if (i >= o->arena.spawnerCount || s->tree == 0) continue;
    if (ready) {
      int32_t k = gatherer_index_of(s->pos.x, s->pos.y);
      if (k < 0 || home->dist[k] == GATHERER_FAR) continue;
      d = home->dist[k];
    } else {
      kfm_fx dx = kfm_fx_abs(s->pos.x - hp.x);
      kfm_fx dy = kfm_fx_abs(s->pos.y - hp.y);
      d = (uint32_t)(kfm_fx_max(dx, dy) / (g_cell > 0 ? g_cell : KFM_FX_ONE));
    }
    cost[i] = (s->intervalTicks > 0 ? s->intervalTicks : 600u) + (2u * d) + 1u;
  }
  for (m = 0;; ++m) {
    uint64_t best = UINT64_MAX;
    int pick = -1;
    for (i = 0; i < KFM_MAX_SPAWNERS; ++i) {
      uint64_t v = (uint64_t)cost[i] * (taken[i] + 1u);
      if (cost[i] != 0 && v < best) {
        best = v;
        pick = i;
      }
    }
    if (pick < 0 || m == rank) return pick;
    ++taken[pick];
  }
}

/* Holding two bananas or more with the throw ready: the button and aim at the
 * nearest visible enemy within the banana's outbound reach whose cell the
 * straight cell line reaches with no wall cell on it; 0 when none. */
static uint8_t gatherer_throw(const kfm_observation* o, kfm_vec me, uint8_t* aim) {
  const kfm_obs_ability* t = &o->self.abilities[1];
  kfm_fx speed = g_banana_speed > 0 ? g_banana_speed : o->rules.bananaSpeed;
  kfm_fx reach = (kfm_fx)(((int64_t)speed * o->rules.bananaOutboundTicks) +
                          o->rules.bananaSpawnOffset - KFM_FX_HALF);
  int64_t best = INT64_MAX;
  int target = -1;
  int i;
  if (o->self.ammo < 2 || t->kind != KFM_ABILITY_THROW || !kfm_ability_ready(o, 1) || !g_ok) {
    return 0;
  }
  for (i = 0; i < KFM_MAX_PLAYERS; ++i) {
    const kfm_obs_player* p = &o->players[i];
    int32_t c0 = gatherer_col_of(me.x);
    int32_t r0 = gatherer_row_of(me.y);
    int32_t c1 = gatherer_col_of(p->posX);
    int32_t r1 = gatherer_row_of(p->posY);
    int32_t dc;
    int32_t dr;
    int32_t err;
    int64_t d;
    int clear = 1;
    if (p->alive == 0 || p->relation != KFM_REL_ENEMY ||
        (p->tags & ((1u << KFM_TAG_INVULNERABLE) | (1u << KFM_TAG_FALLING))) != 0) {
      continue;
    }
    d = kfm_vec_dist(me, kfm_player_pos(p));
    if (d > reach || d >= best || c0 < 0 || r0 < 0 || c1 < 0 || r1 < 0) continue;
    /* Bresenham from the own cell to the enemy's, both ends included. */
    dc = c1 > c0 ? c1 - c0 : c0 - c1;
    dr = r1 > r0 ? r0 - r1 : r1 - r0; /* -|dr| */
    err = dc + dr;
    for (;;) {
      int32_t e2 = 2 * err;
      if (g_solid[((r0 + 1) * g_stride) + c0 + 1] != 0) clear = 0;
      if (!clear || (c0 == c1 && r0 == r1)) break;
      if (e2 >= dr) {
        err += dr;
        c0 += c1 > c0 ? 1 : -1;
      }
      if (e2 <= dc) {
        err += dc;
        r0 += r1 > r0 ? 1 : -1;
      }
    }
    if (!clear) continue;
    best = d;
    target = i;
  }
  if (target < 0) return 0;
  *aim = kfm_dir_to(me, kfm_player_pos(&o->players[target]));
  return t->button;
}

/* An input with the aim and a button of a throw put in. */
static uint64_t gatherer_with_throw(uint64_t in, uint8_t aim, uint8_t button) {
  if (button == 0) return in;
  return (in & ~((uint64_t)0xFFFFu << 16)) | ((uint64_t)aim << 16) | ((uint64_t)button << 24);
}

/* Walks toward a goal from cell `at`: down its field once the field reaches
 * the cell, straight at the goal before; on a goal cell, onto its centre. */
static uint64_t gatherer_go(const kfm_observation* o, int goal, kfm_vec me, int32_t at) {
  gatherer_field* f = &g_fields[goal];
  int d;
  if (!g_ok) return gatherer_toward(me, gatherer_goal_point(o, goal, me));
  if (!f->started) gatherer_seed(f, goal, o);
  gatherer_grow(f, GATHERER_GROW);
  d = gatherer_downhill(f, at);
  if (d >= 0) return gatherer_step(o, me, at, d);
  if (d == -1) {
    kfm_vec c = gatherer_centre_of(at);
    if (o->match.turnTicks > 0 || kfm_vec_dist(me, c) < KFM_FX_ONE / 8) return gatherer_stand();
    return gatherer_toward(me, c);
  }
  return gatherer_toward(me, gatherer_goal_point(o, goal, me));
}

static int gatherer_ignored(const kfm_observation* o, int i, uint32_t tick) {
  return tick < g_ignore_until[i] && g_ignore_spot[i] == gatherer_spot(&o->pickups[i]);
}
static void gatherer_give_up(const kfm_observation* o, int i, uint32_t tick) {
  g_ignore_until[i] = tick + GATHERER_GIVE_UP_TICKS;
  g_ignore_spot[i] = gatherer_spot(&o->pickups[i]);
  g_target = -1;
  g_stuck_ticks = 0;
}

/* Keeps the banana fetched while it lies where it lay, else picks the
 * nearest seen banana pickup on an open cell; whether there is one. */
static int gatherer_pick_banana(const kfm_observation* o, kfm_vec me, uint32_t tick) {
  uint64_t best_sq = UINT64_MAX;
  int best = -1;
  int i;
  if (g_target >= 0) {
    const kfm_obs_pickup* p = &o->pickups[g_target];
    if (p->valid != 0 && p->kind == KFM_PICKUP_BANANA && gatherer_spot(p) == g_target_spot) {
      return 1;
    }
    g_target = -1;
  }
  for (i = 0; i < KFM_MAX_PICKUPS; ++i) {
    const kfm_obs_pickup* p = &o->pickups[i];
    uint64_t d;
    int32_t k;
    if (p->valid == 0 || p->kind != KFM_PICKUP_BANANA || gatherer_ignored(o, i, tick)) continue;
    d = kfm_vec_len_sq(kfm_vec_sub(kfm_v(p->posX, p->posY), me));
    if (d >= best_sq) continue;
    k = g_ok ? gatherer_index_of(p->posX, p->posY) : 0;
    if (k < 0 || (g_ok && g_open[k] == 0)) continue;
    best_sq = d;
    best = i;
  }
  if (best < 0) return 0;
  g_target = best;
  g_target_spot = gatherer_spot(&o->pickups[best]);
  g_stuck_ticks = 0;
  if (g_ok) {
    g_fields[GOAL_BANANA].source = gatherer_index_of(o->pickups[best].posX, o->pickups[best].posY);
    g_fields[GOAL_BANANA].started = 0;
  }
  return 1;
}

/* What the monkey does this tick, but the throw. */
static uint64_t gatherer_move(const kfm_observation* o, kfm_vec me, int32_t at, uint32_t tick) {
  int has_home = o->self.team < o->zones.count && o->self.team < KFM_MAX_ZONES;
  int in_home = 0;
  int tree;
  if (has_home) {
    const kfm_obs_zone* z = &o->zones.zone[o->self.team];
    in_home = gatherer_in_box(me.x, me.y, z->minX, z->minY, z->maxX, z->maxY);
  }
  if (o->self.ammo == 0 || !has_home) g_banking = 0;
  if (o->self.ammo > 0 && has_home && (in_home || o->self.ammo >= o->self.ammoMax)) {
    g_banking = 1;
  }

  if (g_banking) {
    if (in_home) return gatherer_stand();
    return gatherer_go(o, GOAL_HOME, me, at);
  }
  if (o->self.ammo < o->self.ammoMax && gatherer_pick_banana(o, me, tick)) {
    const kfm_obs_pickup* p = &o->pickups[g_target];
    gatherer_field* f = &g_fields[GOAL_BANANA];
    if (g_ok && at == f->source) {
      /* On the banana's cell, not touching it yet: walk onto it (a grid
       * monkey can only wait on the centre), and give it up for a while if
       * that does not work. */
      if (++g_stuck_ticks > GATHERER_STUCK_TICKS) {
        gatherer_give_up(o, g_target, tick);
        return gatherer_stand();
      }
      if (o->match.turnTicks > 0) return gatherer_stand();
      return gatherer_toward(me, kfm_v(p->posX, p->posY));
    }
    /* A field that is complete without reaching the monkey: out of reach. */
    if (g_ok && f->started && f->head >= f->tail && f->dist[at] == GATHERER_FAR &&
        gatherer_downhill(f, at) == -2) {
      gatherer_give_up(o, g_target, tick);
    } else {
      return gatherer_go(o, GOAL_BANANA, me, at);
    }
  }
  if (o->self.ammo > 0 && has_home) {
    g_banking = 1;
    return gatherer_go(o, GOAL_HOME, me, at);
  }
  tree = gatherer_assign(o, me);
  if (tree < 0) return gatherer_stand();
  if (tree != g_tree) {
    g_tree = tree;
    g_fields[GOAL_TREE].started = 0;
    if (g_ok) {
      g_fields[GOAL_TREE].source =
          gatherer_index_of(o->arena.spawners[tree].pos.x, o->arena.spawners[tree].pos.y);
    }
  }
  return gatherer_go(o, GOAL_TREE, me, at);
}

uint64_t kfm_act(uint32_t tick) {
  const kfm_observation* o = &kfm_obs;
  kfm_vec me;
  int32_t at = 0;
  uint8_t aim = 0;
  uint8_t button;
  int i;
  if (!kfm_obs_ok(o) || o->self.alive == 0 ||
      (o->match.phase != KFM_PHASE_PLAYING && o->match.phase != KFM_PHASE_ROUND_STARTING) ||
      kfm_has_tag(o->self.tags, KFM_TAG_FALLING)) {
    g_stuck_ticks = 0;
    return gatherer_stand();
  }
  me = kfm_self_pos(o);
  gatherer_learn_cell(o);
  /* The map and the home field take ticks of their own, the costliest work
   * there is, in the countdown when the bot is there for it. */
  if (g_built < GATHERER_READY) {
    gatherer_field* home = &g_fields[GOAL_HOME];
    if (g_built == 0) {
      gatherer_build(o); /* sets g_built 1, g_ok */
    } else if (!g_ok) {
      g_built = GATHERER_READY;
    } else if (g_built == 1) {
      gatherer_build_steps(0, g_rows / 2);
      g_built = 2;
    } else if (g_built == 2) {
      gatherer_build_steps(g_rows / 2, g_rows);
      g_built = 3;
    } else if (g_built == 3) {
      gatherer_build_walls(o);
      g_built = 4;
    } else if (g_built == 4) {
      gatherer_seed(home, GOAL_HOME, o);
      g_built = 5;
    } else {
      gatherer_grow(home, GATHERER_PREP_GROW);
      if (home->head >= home->tail) g_built = GATHERER_READY;
    }
    return gatherer_stand();
  }
  if (o->match.phase != KFM_PHASE_PLAYING) return gatherer_stand();
  for (i = 0; i < KFM_MAX_BANANAS; ++i) {
    const kfm_obs_banana* b = &o->bananas[i];
    if (b->valid != 0 && b->ownerRelation == KFM_REL_SELF && b->phase < KFM_BANANA_GROUNDED &&
        b->speed > 0) {
      g_banana_speed = b->speed;
    }
  }
  if (g_ok) {
    at = gatherer_index_of(me.x, me.y);
    if (at < 0) return gatherer_toward(me, kfm_v(0, 0));
  }
  button = gatherer_throw(o, me, &aim);
  return gatherer_with_throw(gatherer_move(o, me, at, tick), aim, button);
}
