/* gatherer: the C SDK's example bot for the harvest kind (bot ABI v2,
 * architecture v0.82) and the kind's smoke test: it carries bananas home and
 * does nothing else (no throw, no dash, no catch). Each tick:
 *   1. holding a banana in its own home zone (zone `self.team`), or once set
 *      on going home, it walks home and stands there until its hand is empty
 *      (the kind banks one a beat, modeData word 0);
 *   2. with room in its hand, it walks to the nearest banana pickup it sees
 *      (straight-line nearest; it keeps its choice while the banana lies);
 *   3. else, holding something (a full hand, or nothing to fetch), it sets
 *      off home; empty-handed with nothing to fetch, it walks to the nearest
 *      tree (an arena.spawners entry with `tree` 1) and waits for the fruit.
 * Paths go round walls and pits over a cell map built from arena.walls and
 * arena.hazards: a cell is open when the monkey's circle at its centre lies
 * in the outer box and overlaps no wall, and its centre is on no pit; a
 * diagonal step needs both orthogonal neighbours open (the grid's rule). The
 * cell size is learnt from the own cell at rest in a grid mode (1 unit until
 * then, and in a continuous mode). A goal (the home, the trees, the banana
 * fetched) is a breadth-first distance field from the goal's cells, grown at
 * most GATHERER_GROW cells a tick and walked downhill; until it reaches the
 * monkey's cell it walks straight at the goal. So no tick does more than a
 * bounded amount of work. Build: tools/bots/build_example.sh gatherer. */
#define KFM_BOT_MAIN
#include "../sdk-c/kfm_bot.h"

#define GATHERER_MAX_CELLS 3600u /* the padded map: (cols + 2) x (rows + 2) */
#define GATHERER_GROW 96u        /* cells a field grows by in a tick */
#define GATHERER_STUCK_TICKS 12u /* on a banana's cell this long: give up on it */
#define GATHERER_GIVE_UP_TICKS 600u
#define GATHERER_FAR 0xFFFFu

enum { GOAL_HOME = 0, GOAL_TREES = 1, GOAL_BANANA = 2 };

/* The eight octants (0 east, counter-clockwise; rows grow southward). */
static const int8_t gatherer_dc[8] = {1, 1, 0, -1, -1, -1, 0, 1};
static const int8_t gatherer_dr[8] = {0, -1, -1, -1, 0, 1, 1, 1};

/* The map: cell (col, row) is index (row + 1) * stride + col + 1, inside a
 * closed border, so a neighbour index is never out of range. */
static uint8_t g_open[GATHERER_MAX_CELLS];
static uint8_t g_moves[GATHERER_MAX_CELLS]; /* bit d: the step in octant d is open */
static int32_t g_stride, g_cols, g_rows;
static int32_t g_step[8]; /* index deltas per octant */
static kfm_fx g_cell;     /* the cell size in use, Q16.16 */
static int g_built;       /* the map for g_cell: 0 to build, 1 the cells, 2 the steps too */
static int g_ok;          /* the map fits (else: straight lines) */

/* A breadth-first field: the distance in steps to its sources. */
typedef struct gatherer_field {
  uint16_t dist[GATHERER_MAX_CELLS];
  uint16_t queue[GATHERER_MAX_CELLS];
  uint32_t head, tail;
  int started; /* sources seeded */
  int32_t source; /* GOAL_BANANA: the banana's cell */
} gatherer_field;
static gatherer_field g_fields[3];

static int g_banking;          /* set on going home: until the hand is empty */
static int g_target;           /* the pickup slot fetched, -1 none */
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

/* Closes the cells a box covers: the circle of radius r at a cell's centre
 * overlaps it (r > 0), or the centre lies in it (r == 0, a pit). */
static void gatherer_close_box(const kfm_obs_box* b, kfm_fx r) {
  int32_t c0 = gatherer_col_of(b->minX - r);
  int32_t c1 = gatherer_col_of(b->maxX + r);
  int32_t r0 = gatherer_row_of(b->maxY + r);
  int32_t r1 = gatherer_row_of(b->minY - r);
  int32_t col;
  int32_t row;
  if (c0 < 0) c0 = 0;
  if (r0 < 0) r0 = 0;
  for (row = r0; row <= r1; ++row) {
    kfm_fx y = gatherer_centre_y(row);
    for (col = c0; col <= c1; ++col) {
      kfm_fx x = gatherer_centre_x(col);
      int hit = r > 0 ? gatherer_circle_hits(b, x, y, r)
                      : gatherer_in_box(x, y, b->minX, b->minY, b->maxX, b->maxY);
      if (hit) g_open[((row + 1) * g_stride) + col + 1] = 0;
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
  for (i = 0; i < g_stride * (g_rows + 2); ++i)
    g_open[i] = 0;
  for (row = 0; row < g_rows; ++row) {
    kfm_fx y = gatherer_centre_y(row);
    for (col = 0; col < g_cols; ++col) {
      kfm_fx x = gatherer_centre_x(col);
      g_open[((row + 1) * g_stride) + col + 1] =
          kfm_fx_abs(x) <= a->halfExtents.x - r && kfm_fx_abs(y) <= a->halfExtents.y - r;
    }
  }
  for (i = 0; i < a->wallCount && i < KFM_MAX_WALLS; ++i)
    gatherer_close_box(&a->walls[i], r);
  for (i = 0; i < a->hazardCount && i < KFM_MAX_HAZARDS; ++i) {
    if (a->hazards[i].kind == KFM_HAZARD_PIT) gatherer_close_box(&a->hazards[i].box, 0);
  }
  g_ok = 1;
}

/* The open steps of every cell (a closed cell's too: the monkey may stand on
 * one in a continuous mode, pushed against a wall). */
static void gatherer_build_steps(void) {
  int32_t i;
  int32_t col;
  int32_t row;
  g_built = 2;
  if (!g_ok) return;
  for (i = 0; i < g_stride * (g_rows + 2); ++i)
    g_moves[i] = 0;
  for (row = 1; row <= g_rows; ++row) {
    for (col = 1; col <= g_cols; ++col) {
      const uint8_t* k = &g_open[(row * g_stride) + col];
      uint32_t e = k[1];
      uint32_t n = k[-g_stride];
      uint32_t w = k[-1];
      uint32_t s = k[g_stride];
      g_moves[(row * g_stride) + col] =
          (uint8_t)(e | ((k[1 - g_stride] & e & n) << 1) | (n << 2) |
                    ((k[-1 - g_stride] & n & w) << 3) | (w << 4) | ((k[g_stride - 1] & w & s) << 5) |
                    (s << 6) | ((k[g_stride + 1] & s & e) << 7));
    }
  }
}

static void gatherer_add_source(gatherer_field* f, int32_t k) {
  if (k < 0 || g_open[k] == 0 || f->dist[k] == 0) return;
  f->dist[k] = 0;
  f->queue[f->tail++] = (uint16_t)k;
}

/* Seeds a field with its goal's open cells: the home zone's, the trees', or
 * the banana's. */
static void gatherer_seed(gatherer_field* f, int goal, const kfm_observation* o) {
  int32_t i;
  int32_t col;
  int32_t row;
  f->started = 1;
  f->head = 0;
  f->tail = 0;
  for (i = 0; i < g_stride * (g_rows + 2); ++i)
    f->dist[i] = GATHERER_FAR;
  if (goal == GOAL_BANANA) {
    gatherer_add_source(f, f->source);
  } else if (goal == GOAL_TREES) {
    for (i = 0; i < o->arena.spawnerCount && i < KFM_MAX_SPAWNERS; ++i) {
      const kfm_obs_spawner* s = &o->arena.spawners[i];
      if (s->tree != 0) gatherer_add_source(f, gatherer_index_of(s->pos.x, s->pos.y));
    }
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

/* Grows a field by at most GATHERER_GROW cells. */
static void gatherer_grow(gatherer_field* f) {
  uint32_t budget = GATHERER_GROW;
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
 * monkey: the home zone's centre, the nearest tree, the banana. */
static kfm_vec gatherer_goal_point(const kfm_observation* o, int goal, kfm_vec me) {
  kfm_vec best = me;
  uint64_t best_sq = UINT64_MAX;
  int i;
  if (goal == GOAL_BANANA) {
    if (g_target < 0) return me;
    return kfm_v(o->pickups[g_target].posX, o->pickups[g_target].posY);
  }
  if (goal == GOAL_HOME) {
    const kfm_obs_zone* z;
    if (o->self.team >= o->zones.count || o->self.team >= KFM_MAX_ZONES) return me;
    z = &o->zones.zone[o->self.team];
    return kfm_v((kfm_fx)(((int64_t)z->minX + z->maxX) / 2),
                 (kfm_fx)(((int64_t)z->minY + z->maxY) / 2));
  }
  for (i = 0; i < o->arena.spawnerCount && i < KFM_MAX_SPAWNERS; ++i) {
    const kfm_obs_spawner* s = &o->arena.spawners[i];
    kfm_vec at = kfm_v(s->pos.x, s->pos.y);
    uint64_t d = kfm_vec_len_sq(kfm_vec_sub(at, me));
    if (s->tree == 0 || d >= best_sq) continue;
    best_sq = d;
    best = at;
  }
  return best;
}

/* Walks toward a goal from cell `at`: down its field once the field reaches
 * the cell, straight at the goal before; on a goal cell, onto its centre. */
static uint64_t gatherer_go(const kfm_observation* o, int goal, kfm_vec me, int32_t at) {
  gatherer_field* f = &g_fields[goal];
  int d;
  if (!g_ok) return gatherer_toward(me, gatherer_goal_point(o, goal, me));
  if (!f->started) gatherer_seed(f, goal, o);
  gatherer_grow(f);
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

uint64_t kfm_act(uint32_t tick) {
  const kfm_observation* o = &kfm_obs;
  kfm_vec me;
  int32_t at = 0;
  int has_home;
  int in_home = 0;
  if (!kfm_obs_ok(o) || o->self.alive == 0 || o->match.phase != KFM_PHASE_PLAYING ||
      kfm_has_tag(o->self.tags, KFM_TAG_FALLING)) {
    g_stuck_ticks = 0;
    return gatherer_stand();
  }
  me = kfm_self_pos(o);
  gatherer_learn_cell(o);
  /* The map takes two ticks of their own, the costliest work there is. */
  if (g_built == 0) {
    gatherer_build(o);
    return gatherer_stand();
  }
  if (g_built == 1) {
    gatherer_build_steps();
    return gatherer_stand();
  }
  if (g_ok) {
    at = gatherer_index_of(me.x, me.y);
    if (at < 0) return gatherer_toward(me, kfm_v(0, 0));
  }
  has_home = o->self.team < o->zones.count && o->self.team < KFM_MAX_ZONES;
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
  return gatherer_go(o, GOAL_TREES, me, at);
}
