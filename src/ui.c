/* IHSplay-style launcher menu. Hand-drawn with SDL3 primitives + SDL3_ttf —
 * no UI toolkit. Layout scales from the window size. */
#include "ui.h"

#include <string.h>
#include "ihslib/client.h"
#include "ihs.h"

#define MAX_HOSTS 16

/* ---- live discovery (IHSlib runs it on its own thread) ---- */
static SDL_Mutex *hostLock;
static IHS_HostInfo hosts[MAX_HOSTS];
static int hostCount;

static void OnDiscovered(IHS_Client *client, const IHS_HostInfo *info, void *ctx) {
    (void) client; (void) ctx;
    SDL_LockMutex(hostLock);
    for (int i = 0; i < hostCount; i++) {
        if (hosts[i].clientId == info->clientId) { hosts[i] = *info; goto done; } /* refresh */
    }
    if (hostCount < MAX_HOSTS) hosts[hostCount++] = *info;
    done:
    SDL_UnlockMutex(hostLock);
}

/* Shared with the libretro core; see PlumeResList in ihs.h. Asking for 640x480 on
 * a 16:9 host gets you 640x360, not a 4:3 picture — use the Scaling setting to
 * fill a 4:3 display. */
#define RES   PlumeResList
#define RES_N PlumeResCount

static const char *SCALE_NAMES[] = {"Fit", "Stretch", "Crop"};
#define SCALE_N ((int) SDL_arraysize(SCALE_NAMES))

/* ---- fonts, sized from the live output mode ---- */
static TTF_Font *uiFont, *uiTitleFont, *uiFontSmall;
static int uiFontH;

/* Find a usable TTF: env override, then common system/Recalbox locations. */
static TTF_Font *LoadFont(float pt) {
    const char *paths[] = {
            SDL_getenv("PLUME_FONT"),
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
            "/usr/share/fonts/dejavu/DejaVuSans.ttf",
            "/usr/share/fonts/TTF/DejaVuSans.ttf",
            "/recalbox/share_init/system/.emulationstation/fonts/OpenSans_Regular.ttf",
    };
    for (size_t i = 0; i < SDL_arraysize(paths); i++) {
        if (paths[i] && paths[i][0]) {
            TTF_Font *f = TTF_OpenFont(paths[i], pt);
            if (f) return f;
        }
    }
    return NULL;
}

bool UIEnsureFonts(SDL_Renderer *renderer) {
    int ow, oh;
    SDL_GetRenderOutputSize(renderer, &ow, &oh);
    (void) ow;
    if (oh <= 0) oh = 720;
    if (uiFont && uiTitleFont && oh == uiFontH) return true;
    /* An HDMI mode switch can land after startup: the window opens in one mode
     * and fullscreen settles in another. Layout rects follow the real output
     * every frame; fonts must too, or glyphs overflow their rows and centred
     * text walks off screen. */
    UICloseFonts();
    uiFont = LoadFont(oh / 24.0f);
    uiTitleFont = LoadFont(oh / 13.0f);
    uiFontSmall = LoadFont(oh / 32.0f);
    uiFontH = oh;
    return uiFont && uiTitleFont && uiFontSmall;
}

void UICloseFonts(void) {
    if (uiFont) TTF_CloseFont(uiFont);
    if (uiTitleFont) TTF_CloseFont(uiTitleFont);
    if (uiFontSmall) TTF_CloseFont(uiFontSmall);
    uiFont = uiTitleFont = uiFontSmall = NULL;
    uiFontH = 0;
}

/* ---- draw helpers ---- */
static void FillRect(SDL_Renderer *r, float x, float y, float w, float h,
                     Uint8 cr, Uint8 cg, Uint8 cb, Uint8 ca) {
    SDL_SetRenderDrawColor(r, cr, cg, cb, ca);
    SDL_FRect rect = {x, y, w, h};
    SDL_RenderFillRect(r, &rect);
}

static void Border(SDL_Renderer *r, float x, float y, float w, float h, float t,
                   Uint8 cr, Uint8 cg, Uint8 cb) {
    FillRect(r, x, y, w, t, cr, cg, cb, 255);
    FillRect(r, x, y + h - t, w, t, cr, cg, cb, 255);
    FillRect(r, x, y, t, h, cr, cg, cb, 255);
    FillRect(r, x + w - t, y, t, h, cr, cg, cb, 255);
}

static void Gradient(SDL_Renderer *r, int w, int h) {
    SDL_FColor top = {0.04f, 0.07f, 0.15f, 1}, bot = {0.08f, 0.28f, 0.52f, 1};
    SDL_Vertex v[4] = {
            {{0, 0}, top, {0, 0}}, {{(float) w, 0}, top, {0, 0}},
            {{(float) w, (float) h}, bot, {0, 0}}, {{0, (float) h}, bot, {0, 0}},
    };
    int idx[6] = {0, 1, 2, 0, 2, 3};
    SDL_RenderGeometry(r, NULL, v, 4, idx, 6);
}

/* SDL_RenderGeometry has no antialiasing, so a triangle-fan circle shows its
 * polygon and a hard staircase edge. Every round shape in the UI (icons, play
 * button, the logo's bar caps) routes through Circle(), so fix it here once:
 * rasterise a white disc with a feathered alpha edge at the exact on-screen
 * size, cached per radius. Scaling a single big disc down instead would
 * re-alias — linear filtering only samples 2x2 texels. */
static SDL_Texture *DiscTexture(SDL_Renderer *r, int rad) {
    enum { CACHE = 16 };
    static struct { int rad; SDL_Texture *tex; } cache[CACHE]; /* freed with the renderer */
    static int next;
    for (int i = 0; i < CACHE; i++) {
        if (cache[i].tex && cache[i].rad == rad) return cache[i].tex;
    }
    const int D = rad * 2 + 2; /* 1px margin all round for the feather */
    SDL_Surface *s = SDL_CreateSurface(D, D, SDL_PIXELFORMAT_RGBA32);
    if (!s) return NULL;
    const float c = D / 2.0f - 0.5f, R = (float) rad - 0.5f;
    for (int y = 0; y < D; y++) {
        Uint8 *row = (Uint8 *) s->pixels + (size_t) y * s->pitch;
        for (int x = 0; x < D; x++) {
            float d = SDL_sqrtf((x - c) * (x - c) + (y - c) * (y - c));
            float a = R - d + 1.0f; /* 1px feather */
            a = a < 0 ? 0 : a > 1 ? 1 : a;
            Uint8 *px = row + x * 4;
            px[0] = px[1] = px[2] = 255;
            px[3] = (Uint8) (a * 255.0f);
        }
    }
    SDL_Texture *tex = SDL_CreateTextureFromSurface(r, s);
    SDL_DestroySurface(s);
    if (!tex) return NULL;
    /* Drawn 1:1 on an integer-aligned rect, so no resampling: NEAREST hands the
     * CPU-computed edge to the screen untouched. LINEAR at a subpixel offset
     * smeared the whole disc by half a pixel. */
    SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);
    /* A handful of radii exist per resolution; round-robin is eviction enough. */
    if (cache[next].tex) SDL_DestroyTexture(cache[next].tex);
    cache[next].rad = rad;
    cache[next].tex = tex;
    next = (next + 1) % CACHE;
    return tex;
}

static void Circle(SDL_Renderer *r, float cx, float cy, float rad, SDL_FColor c) {
    int irad = (int) (rad + 0.5f);
    if (irad < 1) return;
    SDL_Texture *disc = DiscTexture(r, irad);
    if (!disc) return;
    SDL_SetTextureColorMod(disc, (Uint8) (c.r * 255), (Uint8) (c.g * 255), (Uint8) (c.b * 255));
    SDL_SetTextureAlphaMod(disc, (Uint8) (c.a * 255));
    float D = (float) (irad * 2 + 2);
    SDL_FRect dst = {SDL_floorf(cx - D / 2 + 0.5f), SDL_floorf(cy - D / 2 + 0.5f), D, D};
    SDL_RenderTexture(r, disc, NULL, &dst);
}

/* The Plume mark ("Aile"): stream bars stacked into a wing, tip in ice.
 * Geometry matches assets/logo.svg (authored on a 128 grid). */
static void Bar(SDL_Renderer *r, float x, float y, float w, float h, SDL_FColor c) {
    /* Pin to the pixel grid so the caps and the body share the same rows. */
    x = SDL_floorf(x + 0.5f);
    y = SDL_floorf(y + 0.5f);
    w = SDL_floorf(w + 0.5f);
    h = SDL_floorf(h + 0.5f);
    Circle(r, x + h / 2, y + h / 2, h / 2, c);
    Circle(r, x + w - h / 2, y + h / 2, h / 2, c);
    SDL_SetRenderDrawColor(r, (Uint8) (c.r * 255), (Uint8) (c.g * 255), (Uint8) (c.b * 255), 255);
    SDL_FRect body = {x + h / 2, y, w - h, h};
    SDL_RenderFillRect(r, &body);
}

/* AA play triangle, rasterised at its exact on-screen size like the discs —
 * the last SDL_RenderGeometry shape with diagonal (so visibly stairstepped)
 * edges. Only one size exists per resolution, so a single slot caches it. */
static void PlayTriangle(SDL_Renderer *r, float cx, float cy, float pr, SDL_FColor c) {
    static SDL_Texture *tex; /* freed with the renderer */
    static int texH;
    int H = (int) (pr + 0.5f), W = (int) (pr * 0.9f + 0.5f);
    if (H < 2 || W < 2) return;
    if (!tex || texH != H) {
        if (tex) { SDL_DestroyTexture(tex); tex = NULL; }
        SDL_Surface *s = SDL_CreateSurface(W, H, SDL_PIXELFORMAT_RGBA32);
        if (!s) return;
        /* alpha = coverage from the signed distance to the closest edge */
        const float ax = 0, ay = 0, bx = 0, by = (float) H, tx = (float) W, ty = H / 2.0f;
        for (int y = 0; y < H; y++) {
            Uint8 *row = (Uint8 *) s->pixels + (size_t) y * s->pitch;
            for (int x = 0; x < W; x++) {
                float px = x + 0.5f, py = y + 0.5f;
                #define EDGE(x0, y0, x1, y1) \
                    (((x1) - (x0)) * (py - (y0)) - ((y1) - (y0)) * (px - (x0))) / \
                    SDL_sqrtf(((x1) - (x0)) * ((x1) - (x0)) + ((y1) - (y0)) * ((y1) - (y0)))
                float d = EDGE(ax, ay, bx, by);
                float d2 = EDGE(bx, by, tx, ty);
                float d3 = EDGE(tx, ty, ax, ay);
                #undef EDGE
                if (d2 > d) d = d2;
                if (d3 > d) d = d3; /* inside is negative; d = distance past the nearest edge */
                float a = 0.5f - d;
                a = a < 0 ? 0 : a > 1 ? 1 : a;
                Uint8 *px8 = row + x * 4;
                px8[0] = px8[1] = px8[2] = 255;
                px8[3] = (Uint8) (a * 255.0f);
            }
        }
        tex = SDL_CreateTextureFromSurface(r, s);
        SDL_DestroySurface(s);
        if (!tex) return;
        SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);
        texH = H;
    }
    SDL_SetTextureColorMod(tex, (Uint8) (c.r * 255), (Uint8) (c.g * 255), (Uint8) (c.b * 255));
    SDL_SetTextureAlphaMod(tex, (Uint8) (c.a * 255));
    SDL_FRect dst = {SDL_floorf(cx - pr * 0.35f + 0.5f), SDL_floorf(cy - H / 2.0f + 0.5f),
                     (float) W, (float) H};
    SDL_RenderTexture(r, tex, NULL, &dst);
}

static void Logo(SDL_Renderer *r, float x, float y, float h) {
    static const float bars[][3] = {{20, 98, 66}, {30, 82, 62}, {40, 66, 56}, {50, 50, 48}, {60, 34, 36}};
    const SDL_FColor accent = {0.24f, 0.67f, 0.96f, 1}, ice = {0.95f, 0.97f, 0.99f, 1};
    float sc = h / 128.0f, bh = 11 * sc;
    for (size_t i = 0; i < SDL_arraysize(bars); i++)
        Bar(r, x + bars[i][0] * sc, y + bars[i][1] * sc, bars[i][2] * sc, bh, accent);
    Bar(r, x + 70 * sc, y + 18 * sc, 20 * sc, bh, ice);
}

/* Render text; returns width. Anchors top-left unless centered in [x,x+boxW]. */
static float Text(SDL_Renderer *r, TTF_Font *f, const char *s, float x, float y,
                  SDL_Color col, float boxW) {
    SDL_Surface *surf = TTF_RenderText_Blended(f, s, strlen(s), col);
    if (!surf) return 0;
    SDL_Texture *tex = SDL_CreateTextureFromSurface(r, surf);
    float tw = (float) surf->w, th = (float) surf->h;
    /* Centering a string wider than its box would start it off screen. */
    if (boxW > 0 && tw < boxW) x += (boxW - tw) / 2;
    /* Glyphs are rendered 1:1; a subpixel offset through LINEAR filtering
     * smears them half a pixel. Pin to the pixel grid, copy untouched. */
    SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);
    SDL_FRect dst = {SDL_floorf(x + 0.5f), SDL_floorf(y + 0.5f), tw, th};
    SDL_RenderTexture(r, tex, NULL, &dst);
    SDL_DestroyTexture(tex);
    SDL_DestroySurface(surf);
    return tw;
}

/* Dispatch face buttons by printed LABEL, not position, so Nintendo/8BitDo
 * layouts (where the bottom button is "B") match the on-screen "(A)/(B)" hints.
 * Falls back to position if the label is unknown. */
static bool BtnLabel(SDL_JoystickID which, Uint8 button,
                     SDL_GamepadButtonLabel want, Uint8 fallbackPos) {
    SDL_Gamepad *gp = SDL_GetGamepadFromID(which);
    SDL_GamepadButtonLabel lbl = gp ? SDL_GetGamepadButtonLabel(gp, button)
                                    : SDL_GAMEPAD_BUTTON_LABEL_UNKNOWN;
    if (lbl == SDL_GAMEPAD_BUTTON_LABEL_UNKNOWN) return button == fallbackPos;
    return lbl == want;
}
static bool BtnIsAccept(SDL_JoystickID w, Uint8 b) {
    return BtnLabel(w, b, SDL_GAMEPAD_BUTTON_LABEL_A, SDL_GAMEPAD_BUTTON_SOUTH);
}
static bool BtnIsBack(SDL_JoystickID w, Uint8 b) {
    return BtnLabel(w, b, SDL_GAMEPAD_BUTTON_LABEL_B, SDL_GAMEPAD_BUTTON_EAST);
}

/* ---- directional input ----
 * SDL repeats held keyboard keys but not gamepad buttons, and it never turns a
 * held stick into discrete steps. Poll both each frame and synthesise repeats. */
#define NAV_DELAY_MS 400 /* before the first repeat */
#define NAV_RATE_MS  110 /* between repeats */
#define NAV_DEADZONE 12000

typedef struct { int x, y; Uint64 nextRepeat; } NavState;

/* Returns true once per discrete step, writing the direction to the out params. */
static bool NavPoll(NavState *s, SDL_Gamepad *pad, int *dx, int *dy) {
    int x = 0, y = 0;
    if (pad) {
        if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_LEFT)) x--;
        if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT)) x++;
        if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_UP)) y--;
        if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN)) y++;
        Sint16 ax = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX);
        Sint16 ay = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY);
        if (!x && ax < -NAV_DEADZONE) x--;
        if (!x && ax > NAV_DEADZONE) x++;
        if (!y && ay < -NAV_DEADZONE) y--;
        if (!y && ay > NAV_DEADZONE) y++;
    }
    /* One axis at a time: diagonals on a stick would otherwise fire two steps. */
    if (x && y) y = 0;

    if (!x && !y) { s->x = s->y = 0; return false; }
    Uint64 now = SDL_GetTicks();
    bool fresh = x != s->x || y != s->y;
    if (!fresh && now < s->nextRepeat) return false;
    s->x = x; s->y = y;
    s->nextRepeat = now + (fresh ? NAV_DELAY_MS : NAV_RATE_MS);
    *dx = x; *dy = y;
    return true;
}

/* ---- settings screen ---- */
/* Modal over the launcher. Up/Down picks a row, Left/Right or A changes it,
 * B/Escape returns. Edits the settings in place. */
static void SettingsScreen(SDL_Renderer *renderer, SDL_Gamepad *pad, int *resIdx,
                           bool *hevc, bool *audio, bool *desktop, int *scale) {
    enum { ROW_RES, ROW_SCALING, ROW_DESKTOP, ROW_HEVC, ROW_AUDIO, ROW_BACK, ROW_N };
    static const char *labels[ROW_N] = {"Resolution", "Scaling", "Desktop mode", "HEVC video", "Audio", "Back"};
    const SDL_Color white = {255, 255, 255, 255}, dim = {180, 195, 215, 255};
    int focus = 0;
    bool running = true;
    NavState nav = {0};

    SDL_PumpEvents();
    SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);

    /* Applies a left/right (or accept) step to the focused row. */
    #define APPLY(d) do { \
        switch (focus) { \
            case ROW_RES: *resIdx = (*resIdx + (d) + RES_N) % RES_N; break; \
            case ROW_SCALING: *scale = (*scale + (d) + SCALE_N) % SCALE_N; break; \
            case ROW_DESKTOP: *desktop = !*desktop; break; \
            case ROW_HEVC: *hevc = !*hevc; break; \
            case ROW_AUDIO: *audio = !*audio; break; \
            case ROW_BACK: running = false; break; \
            default: break; \
        } \
    } while (0)

    while (running) {
        int ndx, ndy;
        if (NavPoll(&nav, pad, &ndx, &ndy)) {
            if (ndy < 0 && focus > 0) focus--;
            else if (ndy > 0 && focus < ROW_N - 1) focus++;
            else if (ndx) APPLY(ndx);
        }
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            int delta = 0;
            switch (e.type) {
                case SDL_EVENT_QUIT: running = false; break;
                case SDL_EVENT_KEY_DOWN:
                    switch (e.key.key) {
                        case SDLK_UP: if (focus > 0) focus--; break;
                        case SDLK_DOWN: if (focus < ROW_N - 1) focus++; break;
                        case SDLK_LEFT: delta = -1; break;
                        case SDLK_RIGHT: delta = 1; break;
                        case SDLK_RETURN: case SDLK_SPACE: delta = 1; break;
                        case SDLK_ESCAPE: running = false; break;
                        default: break;
                    }
                    break;
                case SDL_EVENT_GAMEPAD_BUTTON_DOWN: /* directions come from NavPoll */
                    if (BtnIsAccept(e.gbutton.which, e.gbutton.button)) delta = 1;
                    else if (BtnIsBack(e.gbutton.which, e.gbutton.button)) running = false;
                    break;
                default: break;
            }
            if (delta != 0) APPLY(delta);
        }

        UIEnsureFonts(renderer);
        TTF_Font *font = uiFont, *titleFont = uiTitleFont;
        int W, H;
        SDL_GetRenderOutputSize(renderer, &W, &H);
        float pad_ = H * 0.035f;
        float bt = H * 0.006f; if (bt < 2) bt = 2;

        Gradient(renderer, W, H);
        Text(renderer, titleFont, "Settings", pad_, pad_ * 0.6f, white, 0);

        float rowX = pad_, rowW = W - 2 * pad_, rowH = H * 0.11f, rowY = H * 0.24f;
        for (int i = 0; i < ROW_N; i++) {
            bool f = focus == i;
            FillRect(renderer, rowX, rowY, rowW, rowH, f ? 90 : 60, f ? 115 : 80, f ? 155 : 110, 235);
            if (f) Border(renderer, rowX, rowY, rowW, rowH, bt, 60, 170, 245);
            float ty = rowY + rowH * 0.28f;
            if (i == ROW_BACK) {
                Text(renderer, font, labels[i], rowX, ty, white, rowW);
            } else {
                char value[32];
                const char *text;
                switch (i) {
                    case ROW_RES: text = RES[*resIdx].label; break;
                    case ROW_SCALING: text = SCALE_NAMES[*scale]; break;
                    case ROW_DESKTOP: text = *desktop ? "On" : "Off"; break;
                    case ROW_HEVC: text = *hevc ? "On" : "Off"; break;
                    default: text = *audio ? "On" : "Off"; break;
                }
                SDL_snprintf(value, sizeof(value), f ? "< %s >" : "%s", text);
                Text(renderer, font, labels[i], rowX + rowW * 0.03f, ty, dim, 0);
                Text(renderer, font, value, rowX + rowW * 0.6f, ty, white, 0);
            }
            rowY += rowH + pad_ * 0.4f;
        }

        FillRect(renderer, 0, H - H * 0.08f, W, H * 0.08f, 8, 40, 75, 120);
        Text(renderer, font, "(A) Change   (B) Back", W * 0.66f, H - H * 0.05f, white, 0);
        SDL_RenderPresent(renderer);
    }
    SDL_PumpEvents();
    SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST); /* don't leak the closing B into the menu */
    #undef APPLY
}

/* Launcher focus graph. Indices: 0 = Start tile (left column), 1..n = host rows
 * (right column). The two top-right icons use negative indices so they stay put
 * when a host appears or disappears mid-navigation.
 * Moving is spatial, not list order — Down from Start must not jump columns. */
#define FOCUS_SETTINGS (-1)
#define FOCUS_QUIT     (-2)

static int MenuMove(int focus, int dx, int dy, int n) {
    const int firstHost = n > 0 ? 1 : 0;
    if (focus == 0) {
        if (dx > 0) return firstHost;
        if (dy < 0) return FOCUS_SETTINGS;
    } else if (focus == FOCUS_SETTINGS) {
        if (dx < 0) return FOCUS_QUIT;
        if (dy > 0) return 0;
    } else if (focus == FOCUS_QUIT) {
        if (dx > 0) return FOCUS_SETTINGS;
        if (dy > 0) return firstHost;
    } else { /* a host row */
        if (dx < 0) return 0;
        if (dy < 0) return focus > 1 ? focus - 1 : FOCUS_SETTINGS;
        if (dy > 0 && focus < n) return focus + 1;
    }
    return focus;
}

/* ---- pairing screen ---- */
typedef struct { volatile bool done, ok; } PairState;

static void POnSuccess(IHS_Client *c, const IHS_HostInfo *h, uint64_t steamId, void *ctx) {
    (void) h; (void) steamId;
    PairState *s = ctx; s->ok = true; s->done = true;
    IHS_ClientStop(c);
}
static void POnFailed(IHS_Client *c, const IHS_HostInfo *h, IHS_AuthorizationResult r, void *ctx) {
    (void) h; (void) r;
    PairState *s = ctx; s->done = true;
    IHS_ClientStop(c);
}
static void POnProgress(IHS_Client *c, const IHS_HostInfo *h, void *ctx) { (void) c; (void) h; (void) ctx; }

bool PairScreen(SDL_Renderer *renderer, const IHS_ClientConfig *config,
                const IHS_HostInfo *host) {
    /* Not SDL_IOFromFile: it rejects character devices ("/dev/urandom is not a
     * regular file or pipe"), so the read never happened and every PIN came out
     * 0000. PlumeMakePin uses plain fopen, which is why the libretro core was
     * never affected. */
    char pin[5];
    PlumeMakePin(pin);

    IHS_Client *client = IHS_ClientCreate(config);
    PairState st = {0};
    IHS_ClientAuthorizationCallbacks cb = {.progress = POnProgress, .success = POnSuccess, .failed = POnFailed};
    IHS_ClientSetAuthorizationCallbacks(client, &cb, &st);
    IHS_ClientStartDiscovery(client, 0);              /* keep the worker thread alive */
    IHS_ClientAuthorizationRequest(client, host, pin);

    /* Open connected gamepads so "B" can cancel on a controller-only device. */
    SDL_Gamepad *pads[8]; int np = 0;
    int nj; SDL_JoystickID *ids = SDL_GetGamepads(&nj);
    if (ids) { for (int i = 0; i < nj && np < 8; i++) pads[np++] = SDL_OpenGamepad(ids[i]); SDL_free(ids); }

    const SDL_Color white = {255, 255, 255, 255}, dim = {180, 195, 215, 255};
    char line[128];
    SDL_snprintf(line, sizeof(line), "Pairing with %s", host->hostname);

    bool cancelled = false;
    while (!st.done && !cancelled) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT ||
                (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE) ||
                (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && BtnIsBack(e.gbutton.which, e.gbutton.button)))
                cancelled = true;
        }
        UIEnsureFonts(renderer);
        TTF_Font *font = uiFont, *titleFont = uiTitleFont;
        int W, H;
        SDL_GetRenderOutputSize(renderer, &W, &H);
        Gradient(renderer, W, H);
        Text(renderer, font, line, 0, H * 0.22f, white, W);
        Text(renderer, font, "Enter this PIN in Steam on the host:", 0, H * 0.36f, dim, W);
        Text(renderer, titleFont, pin, 0, H * 0.46f, white, W);
        Text(renderer, font, "(B) Cancel", 0, H * 0.78f, dim, W);
        SDL_RenderPresent(renderer);
    }

    for (int i = 0; i < np; i++) if (pads[i]) SDL_CloseGamepad(pads[i]);
    IHS_ClientStopDiscovery(client);
    IHS_ClientStop(client);
    IHS_ClientThreadedJoin(client);
    IHS_ClientDestroy(client);
    return st.ok;
}

/* ---- menu ---- */
UIAction RunMenu(SDL_Window *window, SDL_Renderer *renderer,
                 const IHS_ClientConfig *config, UIResult *out) {
    hostLock = SDL_CreateMutex();
    hostCount = 0;

    IHS_Client *client = IHS_ClientCreate(config);
    IHS_ClientDiscoveryCallbacks cb = {.discovered = OnDiscovered};
    IHS_ClientSetDiscoveryCallbacks(client, &cb, NULL);
    IHS_ClientStartDiscovery(client, 2000); /* re-broadcast every 2s */

    /* Drop input left over from however we were launched (e.g. the button ES
     * used to start us), and ignore "quit" actions for a short grace period so
     * a late-arriving launch event can't close the menu on frame one. */
    SDL_PumpEvents();
    SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);
    Uint64 startTicks = SDL_GetTicks();

    /* Open whatever is already plugged in: after a stream the HID provider has
     * closed the gamepads it owned, and no ADDED event will come for them. */
    SDL_Gamepad *pad = NULL;
    const char *padName = NULL;
    int nj;
    SDL_JoystickID *ids = SDL_GetGamepads(&nj);
    if (ids) {
        if (nj > 0) { pad = SDL_OpenGamepad(ids[0]); padName = pad ? SDL_GetGamepadName(pad) : NULL; }
        SDL_free(ids);
    }

    int resIdx = PlumeResIndex(out->width, out->height);
    const int resIdx0 = resIdx;
    bool hevc = out->hevc, audio = out->audio, desktop = out->desktop;
    int scale = out->scale;

    /* focus: 0 = Start tile, 1..n = host rows, n+1 = settings icon, n+2 = quit icon */
    int focus = 0;
    int sel = 0;     /* selected host index */
    NavState nav = {0};
    UIAction action = UI_QUIT;
    const SDL_Color white = {255, 255, 255, 255}, dim = {180, 195, 215, 255};

    bool running = true;
    while (running) {
        int n;
        SDL_LockMutex(hostLock);
        n = hostCount;
        SDL_UnlockMutex(hostLock);
        /* --- layout (before input, so the icons can be hit-tested) --- */
        UIEnsureFonts(renderer);
        TTF_Font *font = uiFont;
        int W, H;
        SDL_GetRenderOutputSize(renderer, &W, &H);
        float pad_ = H * 0.035f;
        float bt = H * 0.006f; if (bt < 2) bt = 2; /* focus border, min 2px at low res */
        float br = H * 0.032f, by = pad_ + br * 0.4f, gap = br * 2.6f;
        float cx = W - pad_ - br;                       /* settings icon centre */
        float icy = by + br;
        float qx = cx - 2 * gap;                        /* quit icon centre */

        /* --- input --- */
        bool grace = SDL_GetTicks() - startTicks < 350; /* ignore quit right after launch */
        int ndx, ndy;
        if (NavPoll(&nav, pad, &ndx, &ndy)) focus = MenuMove(focus, ndx, ndy, n);
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
                case SDL_EVENT_QUIT:
                    if (!grace) running = false;
                    break;
                case SDL_EVENT_GAMEPAD_ADDED:
                    if (!pad) { pad = SDL_OpenGamepad(e.gdevice.which); padName = SDL_GetGamepadName(pad); }
                    break;
                case SDL_EVENT_GAMEPAD_REMOVED:
                    if (pad) { SDL_CloseGamepad(pad); pad = NULL; padName = NULL; }
                    break;
                case SDL_EVENT_MOUSE_BUTTON_DOWN: {
                    float dx = e.button.x - cx, dy = e.button.y - icy;
                    float qdx = e.button.x - qx;
                    if (dx * dx + dy * dy <= br * br) { focus = FOCUS_SETTINGS; goto activate; }
                    if (qdx * qdx + dy * dy <= br * br) { focus = FOCUS_QUIT; goto activate; }
                    break;
                }
                case SDL_EVENT_KEY_DOWN: /* SDL already repeats held keys */
                    switch (e.key.key) {
                        case SDLK_UP: focus = MenuMove(focus, 0, -1, n); break;
                        case SDLK_DOWN: focus = MenuMove(focus, 0, 1, n); break;
                        case SDLK_LEFT: focus = MenuMove(focus, -1, 0, n); break;
                        case SDLK_RIGHT: focus = MenuMove(focus, 1, 0, n); break;
                        case SDLK_RETURN: case SDLK_SPACE: goto activate;
                        case SDLK_ESCAPE: if (!grace) running = false; break;
                        default: break;
                    }
                    break;
                case SDL_EVENT_GAMEPAD_BUTTON_DOWN: /* directions come from NavPoll */
                    if (BtnIsAccept(e.gbutton.which, e.gbutton.button)) goto activate;
                    else if (!grace && BtnIsBack(e.gbutton.which, e.gbutton.button)) running = false;
                    break;
                default: break;
            }
            continue;
            activate:
            if (focus == FOCUS_QUIT) { action = UI_QUIT; running = false; break; }
            if (focus == FOCUS_SETTINGS) {
                SettingsScreen(renderer, pad, &resIdx, &hevc, &audio, &desktop, &scale);
                nav = (NavState) {0}; /* the stick may still be held; don't carry it back */
                break;
            }
            if (focus >= 1 && focus <= n) sel = focus - 1;
            if (n > 0) { action = UI_START; running = false; }
            break;
        }
        if (focus >= 1 && focus <= n && n > 0) sel = focus - 1; /* focus on a host row -> select it */
        if (focus > n) focus = n > 0 ? n : 0;                   /* clamp if a host vanished */

        /* --- render --- */
        Gradient(renderer, W, H);
        /* The mark ink spans y 16..108 of its 128 grid, so the box overshoots a
         * touch; sized to sit where the old wordmark title did. */
        Logo(renderer, pad_ * 0.6f, pad_ * 0.2f, H * 0.16f);

        /* top-right circular buttons: settings (accent), help, quit */
        if (focus == FOCUS_SETTINGS) Circle(renderer, cx, icy, br * 1.22f, (SDL_FColor) {0.24f, 0.67f, 0.96f, 1});
        if (focus == FOCUS_QUIT) Circle(renderer, qx, icy, br * 1.22f, (SDL_FColor) {0.94f, 0.35f, 0.35f, 1});
        Circle(renderer, cx, icy, br, (SDL_FColor) {0.13f, 0.6f, 0.92f, 1});           /* settings */
        Circle(renderer, cx - gap, icy, br, (SDL_FColor) {0.35f, 0.4f, 0.5f, 1});      /* help */
        Circle(renderer, qx, icy, br, (SDL_FColor) {0.35f, 0.4f, 0.5f, 1});            /* quit */
        Text(renderer, font, "\xe2\x89\xa1", cx - br, by + br * 0.35f, white, br * 2); /* U+2261, gear stand-in */
        Text(renderer, font, "?", cx - gap - br, by + br * 0.35f, white, br * 2);
        Text(renderer, font, "X", qx - br, by + br * 0.35f, white, br * 2);

        float top = H * 0.19f;
        float tileW = W * 0.29f, tileH = H * 0.37f, tileX = pad_, tileY = top;

        /* Start Streaming tile */
        bool f0 = focus == 0;
        FillRect(renderer, tileX, tileY, tileW, tileH, f0 ? 90 : 60, f0 ? 115 : 80, f0 ? 155 : 110, 235);
        if (f0) Border(renderer, tileX, tileY, tileW, tileH, bt, 60, 170, 245);
        float pcx = tileX + tileW / 2, pcy = tileY + tileH * 0.42f, pr = tileH * 0.16f;
        Circle(renderer, pcx, pcy, pr, (SDL_FColor) {1, 1, 1, 1});
        PlayTriangle(renderer, pcx, pcy, pr, (SDL_FColor) {0.04f, 0.07f, 0.15f, 1});
        Text(renderer, font, "Start Streaming", tileX, tileY + tileH * 0.72f, white, tileW);

        /* current settings, read-only under the Start tile */
        char summary[64];
        SDL_snprintf(summary, sizeof(summary), "%s  %s  %s  %s  %s", RES[resIdx].label,
                     SCALE_NAMES[scale], desktop ? "Desktop" : "Game",
                     hevc ? "HEVC" : "H264", audio ? "Audio" : "Muted");
        Text(renderer, uiFontSmall, summary, tileX, tileY + tileH + pad_ * 0.4f, dim, tileW);

        /* right column: host rows + gamepad status */
        float rowX = tileX + tileW + pad_, rowW = W - rowX - pad_;
        float rowH = H * 0.1f, rowY = top;
        SDL_LockMutex(hostLock);
        for (int i = 0; i < n; i++) {
            bool fi = focus == i + 1;
            FillRect(renderer, rowX, rowY, rowW, rowH, fi ? 90 : 60, fi ? 115 : 80, fi ? 155 : 110, 235);
            if (fi) Border(renderer, rowX, rowY, rowW, rowH, bt, 60, 170, 245);
            if (i == sel) FillRect(renderer, rowX, rowY, rowW * 0.012f, rowH, 60, 170, 245, 255);
            Text(renderer, font, hosts[i].hostname, rowX + rowW * 0.04f, rowY + rowH * 0.28f, white, 0);
            rowY += rowH + pad_ * 0.5f;
        }
        SDL_UnlockMutex(hostLock);
        if (n == 0) {
            FillRect(renderer, rowX, rowY, rowW, rowH, 50, 65, 90, 200);
            Text(renderer, font, "Searching for hosts...", rowX + rowW * 0.04f, rowY + rowH * 0.28f, dim, 0);
            rowY += rowH + pad_ * 0.5f;
        }
        FillRect(renderer, rowX, rowY, rowW, rowH, 50, 62, 85, 200);
        Text(renderer, font, pad ? padName : "No gamepad connected",
             rowX + rowW * 0.04f, rowY + rowH * 0.28f, dim, 0);

        /* bottom hint bar (right-aligned) */
        FillRect(renderer, 0, H - H * 0.08f, W, H * 0.08f, 8, 40, 75, 120);
        Text(renderer, font, "(A) Open   (B) Back", W * 0.68f, H - H * 0.05f, white, 0);

        SDL_RenderPresent(renderer);
    }

    IHS_ClientStopDiscovery(client);
    IHS_ClientStop(client);
    IHS_ClientThreadedJoin(client);
    IHS_ClientDestroy(client);
    if (pad) SDL_CloseGamepad(pad);

    /* Only the Resolution row owns these four. Writing them back unconditionally
     * clobbered an fps or kbps hand-edited into settings.conf — and any resolution
     * absent from RES, which the loop above resolves to 1080p. */
    if (resIdx != resIdx0) {
        out->width = RES[resIdx].w;
        out->height = RES[resIdx].h;
        out->fps = RES[resIdx].fps;
        out->kbps = RES[resIdx].kbps;
    }
    out->hevc = hevc;
    out->desktop = desktop;
    out->scale = scale;
    out->audio = audio;
    /* Discovery thread is joined, so hosts[] is now ours to read without the lock. */
    if (action == UI_START) out->host = hosts[sel < hostCount ? sel : 0];
    SDL_DestroyMutex(hostLock);
    return action;
}
