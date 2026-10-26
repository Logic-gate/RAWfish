/*
 * ctl_shoot.h -- what a press of the shutter becomes, in the controller
 * (docs/ARCHITECTURE.md, step 6). Pure: the request, the exposure and focus
 * states in, the engine's capture lines out (one to three).
 *
 * Rules (from the page's shoot(), ui 128):
 * - a pair the controller holds (a wheel pinned, or a spot held in
 *   automatic) goes with the photo (exp= iso=), so the photo is exposed as
 *   the viewfinder shows; otherwise the camera's AE decides at the shot;
 * - a manual focus restates its distance (focus=); any other mode keeps the
 *   lens where the live focus put it (no re-scan);
 * - a shutter beyond the single-exposure ceiling, with a pair, is a STACK:
 *   film = the photo-equivalent seconds, the engine adds sub-frames at the
 *   ceiling; stk_denoise = extra frames divided out;
 * - bracket "0" in JPEG is a real burst of three (one capture call, back to
 *   back); in raw three shots in turn; bracket N: three real exposures at
 *   -N, 0, +N stops around the pair (ISO kept), clamped to the single
 *   exposure range;
 * - JPEG lines carry the quality and the photo's processing modes; raw lines
 *   carry neither.
 */
#ifndef INDRAW_CTL_SHOOT_H
#define INDRAW_CTL_SHOOT_H

#include "ctl_exposure.h"
#include "ctl_focus.h"

typedef struct {
    char name[64];              /* the file name prefix */
    int raw;                    /* 1 raw, 0 jpeg */
    int quality;                /* JPEG quality */
    char noise[24], edge[24];   /* the photo's processing modes (jpeg), "" = none */
    int bracket;                /* -1 off, 0 = x3 same exposure, 1..3 stops */
    int denoise;                /* stacks: 1, 2 or 4 */
    double gap;                 /* x3 burst: frame spacing, s (0 = fastest) */
} ctl_shoot_req_t;

#define CTL_SHOOT_MAX_LINES 3
#define CTL_SHOOT_LINE 256

/* Fills lines[] with the engine commands and returns their number (0 if the
 * request is invalid). shutter_max_ns / exp_min_ns: the camera's limits. */
int ctl_shoot_lines(const ctl_shoot_req_t *req, const ctl_exp_state_t *xs,
                    double shutter_max_ns, double exp_min_ns,
                    const ctl_focus_state_t *fs, char lines[CTL_SHOOT_MAX_LINES][CTL_SHOOT_LINE]);

#endif
