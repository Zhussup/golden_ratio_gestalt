// golden_ratio.cpp — animated golden ratio / Fibonacci spiral
//
// The classic construction: golden rectangle -> squares of Fibonacci size
// (1, 1, 2, 3, 5, 8, ...) attach around a growing rectangle while a
// quarter-arc spiral draws through each square. A live readout shows
// F(n+1)/F(n) converging to phi = 1.6180339887...
//
// Build:  g++ -O2 -std=c++17 golden_ratio.cpp $(pkg-config --libs glfw3) -lGL -o golden_ratio
// Run:    ./golden_ratio
// Keys:   Space pause · R restart · Up/Down speed · Esc quit
// Dump:   ./golden_ratio --dump 19 0.92 [dir] [width] [height]  (PPM frames, no window)

#include <GLFW/glfw3.h>
#include <GL/gl.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <array>
#include <map>
#include <algorithm>

#include <sys/stat.h>

// ------------------------------------------------------------------ math
static const double PI = 3.14159265358979323846;

struct V { double x, y; };
static inline V operator+(V a, V b){ return {a.x+b.x, a.y+b.y}; }
static inline V operator-(V a, V b){ return {a.x-b.x, a.y-b.y}; }
static inline V operator*(V a, double k){ return {a.x*k, a.y*k}; }
static inline V operator*(double k, V a){ return {k*a.x, k*a.y}; }
static inline V rotcw(V v){ return {v.y, -v.x}; }   // rotate 90 deg clockwise

static double clamp01(double x){ return x < 0 ? 0 : x > 1 ? 1 : x; }
static double easeOut(double x){ x = clamp01(x); return 1 - (1-x)*(1-x)*(1-x); }

// ---------------------------------------------------------- construction
static const int NS = 11;                 // squares 1,1,2,3,5,8,13,21,34,55,89

struct Sq  { double x0, y0, x1, y1; int k; double s; };
struct Arc { V c; double r, th0; int sq; };

static std::vector<Sq>  squares;
static std::vector<Arc> arcs;
static double FIB[16];
static double wx0, wy0, wx1, wy1;         // world bounding box

// Square attachment cycle (verified for tangent continuity, winding cw):
//   S1 at origin, S2 to the RIGHT, then for k>=3 cycle U, R, D, L.
static void build()
{
    FIB[1] = FIB[2] = 1;
    for (int i = 3; i <= NS + 1; i++) FIB[i] = FIB[i-1] + FIB[i-2];

    squares.push_back({0, 0, 1, 1, 1, 1});
    {   // S2
        Sq r = squares.back();
        squares.push_back({r.x1, r.y1 - 1, r.x1 + 1, r.y1, 2, 1});
    }
    for (int k = 3; k <= NS; k++) {
        double s = FIB[k];
        Sq r = squares.back();
        Sq n{0, 0, 0, 0, k, s};
        switch ((k - 3) % 4) {
            case 0: n = {r.x0,     r.y1,     r.x0 + s, r.y1 + s, k, s}; break; // up
            case 1: n = {r.x1,     r.y1 - s, r.x1 + s, r.y1,     k, s}; break; // right
            case 2: n = {r.x1 - s, r.y0 - s, r.x1,     r.y0,     k, s}; break; // down
            case 3: n = {r.x0 - s, r.y0,     r.x0,     r.y0 + s, k, s}; break; // left
        }
        squares.push_back(n);
    }
    // arcs in spiral traversal order (S2's arc, then S1's, then S3, S4, ...)
    V p{2, 1}, h{0, -1};
    for (int j = 0; j < NS; j++) {
        double r = FIB[j + 1];
        V c = p + rotcw(h) * r;                       // center right of motion
        double th0 = atan2(p.y - c.y, p.x - c.x);
        int sq = (j == 0) ? 2 : (j == 1) ? 1 : j + 1; // square index (0-based)
        arcs.push_back({c, r, th0, sq - 1});
        p = c + V{cos(th0 - PI/2), sin(th0 - PI/2)} * r;
        h = V{sin(th0 - PI/2), -cos(th0 - PI/2)};     // cw tangent keeps turning right
    }
    wx0 = wy0 = 1e18; wx1 = wy1 = -1e18;
    for (auto &q : squares) {
        wx0 = fmin(wx0, q.x0); wy0 = fmin(wy0, q.y0);
        wx1 = fmax(wx1, q.x1); wy1 = fmax(wy1, q.y1);
    }
}

// -------------------------------------------------------------- timeline
static const double T0      = 2.0;    // first arc starts
static const double ARC_DUR = 0.82;
static const double ARC_GAP = 0.10;
static const double HOLD    = 4.0;
static const double FADE    = 0.9;
static const double BLACK   = 0.45;

static double arcStart(int j) { return T0 + j * (ARC_DUR + ARC_GAP); }
static double arcEnd(int j)   { return arcStart(j) + ARC_DUR; }
static const double LAST_END  = T0 + (NS - 1) * (ARC_DUR + ARC_GAP) + ARC_DUR;
static const double LOOP      = LAST_END + HOLD + FADE + BLACK;

static float gFade = 1;
static bool  gPaused = false;

// ------------------------------------------------------------------ font
struct Glyph { double wf; std::vector<std::array<double,4>> s; };
static std::map<char, Glyph> font;

static void segList(Glyph &g, std::initializer_list<std::array<double,4>> l)
{
    for (auto &e : l) g.s.push_back(e);
}

static void initFont()
{
    auto add = [](char ch, double wf, std::initializer_list<std::array<double,4>> l){
        Glyph g{wf, {}}; segList(g, l); font[ch] = g;
    };
    const double gi = 0.06, gm = 0.52;
    std::array<double,4> SA {gi, 1, 1-gi, 1}, SB {1, 1-gi, 1, gm},
                          SC {1, 0.48, 1, gi}, SD {gi, 0, 1-gi, 0},
                          SE {0, 0.48, 0, gi}, SF {0, 1-gi, 0, gm},
                          SG {gi, gm, 1-gi, gm};
    auto digit = [&](char ch, std::initializer_list<std::array<double,4>> l){
        add(ch, 0.62, l);
    };
    digit('0', {SA, SB, SC, SD, SE, SF});
    digit('1', {{0.5, 0.02, 0.5, 1.0}, {0.16, 0.85, 0.5, 1.0}});  // stem + flag
    digit('2', {SA, SB, SG, SE, SD});
    digit('3', {SA, SB, SG, SC, SD});
    digit('4', {SF, SG, SB, SC});
    digit('5', {SA, SF, SG, SC, SD});
    digit('6', {SA, SF, SG, SE, SD, SC});
    digit('7', {SA, SB, SC});
    digit('8', {SA, SB, SC, SD, SE, SF, SG});
    digit('9', {SA, SB, SC, SD, SF, SG});

    add('F', 0.55, {{0.03,0,0.03,1}, {0.03,1,0.92,1}, {0.03,0.55,0.70,0.55}});
    add('n', 0.55, {{0.03,0,0.03,0.66}, {0.03,0.66,0.90,0.66}, {0.90,0.66,0.90,0}});
    add('.', 0.30, {{0.30,0.02,0.66,0.02}});
    add('*', 0.30, {{0.30,0.45,0.66,0.53}});          // mid separator
    add('/', 0.40, {{0.10,0,0.88,1}});
    add('+', 0.55, {{0.40,0.06,0.40,0.94}, {0.05,0.5,0.75,0.5}});
    add('=', 0.60, {{0.02,0.70,0.98,0.70}, {0.02,0.32,0.98,0.32}});
    add('-', 0.55, {{0.05,0.5,0.95,0.5}});
    add('V', 0.85, {{0.05,0.5,0.20,0.12}, {0.20,0.12,0.45,0.95}, {0.45,0.95,0.98,0.95}});
    add('>', 0.75, {{0,0.5,0.62,0.5}, {0.62,0.5,0.40,0.70}, {0.62,0.5,0.40,0.30}});
    add('~', 0.85, {{0,0.70,0.33,0.76}, {0.33,0.76,0.66,0.64}, {0.66,0.64,1,0.70},
                     {0,0.38,0.33,0.44}, {0.33,0.44,0.66,0.32}, {0.66,0.32,1,0.38}});
    add(' ', 0.42, {});

    // phi: ellipse + stem
    {
        Glyph g{0.75, {}};
        g.s.push_back({0.47, 0.0, 0.47, 1.0});
        const int NP = 18;
        for (int i = 0; i < NP; i++) {
            double a = 2*PI*i/NP, b = 2*PI*(i+1)/NP;
            g.s.push_back({0.47 + 0.30*cos(a), 0.52 + 0.42*sin(a),
                           0.47 + 0.30*cos(b), 0.52 + 0.42*sin(b)});
        }
        font['&'] = g;   // '&' is our stand-in for phi
    }
    // parentheses: elliptical arcs
    auto paren = [&](char ch, int mirror){
        Glyph g{0.45, {}};
        const int NP = 12;
        for (int i = 0; i < NP; i++) {
            double a = 0.62 + (2*PI - 1.24) * i/NP;
            double b = 0.62 + (2*PI - 1.24) * (i+1)/NP;
            double fx = mirror;
            g.s.push_back({0.62 + fx*0.20*cos(a), 0.5 + 0.52*sin(a),
                           0.62 + fx*0.20*cos(b), 0.5 + 0.52*sin(b)});
        }
        font[ch] = g;
    };
    paren('(', 1); paren(')', -1);
}

static double textW(const char *s, double size)
{
    double w = 0;
    for (const char *p = s; *p; p++) {
        auto it = font.find(*p);
        double wf = (it != font.end()) ? it->second.wf : 0.6;
        w += (wf + 0.16) * size;
    }
    return w - 0.16 * size;
}

static void drawText(const char *s, double x, double y, double size,
                     float r, float g, float b, float a, int align_ = 0)
{
    if (a <= 0.003f) return;
    double w = textW(s, size);
    if (align_ == 1) x -= w / 2;
    if (align_ == 2) x -= w;
    glColor4f(r, g, b, a * gFade);
    glBegin(GL_LINES);
    for (const char *p = s; *p; p++) {
        auto it = font.find(*p);
        if (it != font.end()) {
            double sx = it->second.wf * size, sy = size;
            for (auto &e : it->second.s) {
                glVertex2d(x + e[0]*sx, y + e[1]*sy);
                glVertex2d(x + e[2]*sx, y + e[3]*sy);
            }
            x += (it->second.wf + 0.16) * size;
        } else {
            x += 0.6 * size;
        }
    }
    glEnd();
}

// --------------------------------------------------------------- drawing
static void setC(float r, float g, float b, float a)
{
    glColor4f(r, g, b, a * gFade);
}

static void drawDisc(V c, double r, float cr, float cg, float cb, float a)
{
    if (a * gFade <= 0.003f) return;
    setC(cr, cg, cb, a);
    glBegin(GL_TRIANGLE_FAN);
    glVertex2d(c.x, c.y);
    for (int i = 0; i <= 20; i++) {
        double t = 2*PI*i/20;
        glVertex2d(c.x + r*cos(t), c.y + r*sin(t));
    }
    glEnd();
}

static void strokeArc(V c, double r, double th0, double span, int steps)
{
    glBegin(GL_LINE_STRIP);
    for (int i = 0; i <= steps; i++) {
        double th = th0 - span * i / steps;
        glVertex2d(c.x + r*cos(th), c.y + r*sin(th));
    }
    glEnd();
}

static void rectWalkPath(double x0, double y0, double x1, double y1, double t)
{
    V c[5] = {{x0,y1}, {x1,y1}, {x1,y0}, {x0,y0}, {x0,y1}};
    double lens[4] = {x1-x0, y1-y0, x1-x0, y1-y0};
    double total = 2 * (x1-x0 + y1-y0);
    double d = clamp01(t) * total;
    glBegin(GL_LINE_STRIP);
    glVertex2d(c[0].x, c[0].y);
    for (int e = 0; e < 4 && d > 1e-9; e++) {
        double take = fmin(d, lens[e]); d -= take;
        double f = take / lens[e];
        V p = c[e] + (c[e+1] - c[e]) * f;
        glVertex2d(p.x, p.y);
    }
    glEnd();
}

// ---------------------------------------------------------------- render
static void render(double t, int W, int H)
{
    double fade = 1;
    if (t < 0.5) fade = t / 0.5;
    if (t > LAST_END + HOLD) fade = clamp01(1 - (t - LAST_END - HOLD) / FADE);
    gFade = (float)fade;

    double sc = fmin((W - 2*58.0) / (wx1 - wx0), (H - 2*62.0) / (wy1 - wy0));
    double ox = W / (2*sc), oy = H / (2*sc);
    double cx = (wx0 + wx1) / 2, cy = (wy0 + wy1) / 2;
    auto w2p = [&](V w){ return V{(w.x - cx)*sc + W*0.5, (w.y - cy)*sc + H*0.5}; };
    double marginL = (W - (wx1 - wx0)*sc) / 2;

    double rectWalk = clamp01((t - 0.25) / 1.05);
    double holdT = t - LAST_END;
    bool holding = holdT >= 0 && t < LAST_END + HOLD + 0.001;
    double pulse = holding ? 0.5 + 0.5*sin(2*PI*holdT/3.6) : 0;

    glViewport(0, 0, W, H);
    glClearColor(0.055f, 0.050f, 0.042f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_LINE_SMOOTH);
    glEnable(GL_MULTISAMPLE);

    // ---- world pass
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glOrtho(cx - ox, cx + ox, cy - oy, cy + oy, -1, 1);
    glMatrixMode(GL_MODELVIEW);  glLoadIdentity();

    // square fills
    for (int j = 0; j < NS; j++) {
        const Arc &a = arcs[j];
        const Sq  &q = squares[a.sq];
        double ap = easeOut(clamp01((t - (arcStart(j) - 0.38)) / 0.30));
        if (ap <= 0) continue;
        double mix = (q.k % 4) / 3.0;
        setC((float)(0.86 + 0.13*mix), (float)(0.62 + 0.16*mix),
             (float)(0.14 + 0.11*mix), (float)(0.115 - 0.003*q.k));
        glBegin(GL_QUADS);
        glVertex2d(q.x0, q.y0); glVertex2d(q.x1, q.y0);
        glVertex2d(q.x1, q.y1); glVertex2d(q.x0, q.y1);
        glEnd();
        setC(0.95, 0.75, 0.30, (float)(0.38 * ap));
        glLineWidth(1.4f);
        glBegin(GL_LINE_LOOP);
        glVertex2d(q.x0, q.y0); glVertex2d(q.x1, q.y0);
        glVertex2d(q.x1, q.y1); glVertex2d(q.x0, q.y1);
        glEnd();
    }

    // square outlines are drawn above; now the spiral arcs
    for (int j = 0; j < NS; j++) {
        const Arc &a = arcs[j];
        double st = arcStart(j);
        double u = clamp01((t - st) / ARC_DUR);
        if (u <= 0) continue;
        double span = u * PI / 2;
        bool drawing = u < 1;
        double aA = holding ? 0.75 + 0.18*pulse : (drawing ? 1.0 : 0.82);
        int steps = (int)fmin(180, fmax(12, a.r * sc / 5.0));
        setC(1.0f, drawing ? 0.88f : 0.83f, drawing ? 0.34f : 0.26f, (float)aA);
        glLineWidth(3.2f);
        strokeArc(a.c, a.r, a.th0, span, steps);
    }

    // travelling highlight during hold
    if (holding) {
        double q = fmod(holdT * 0.5, NS);
        int qi = (int)q;
        double uq = q - qi;
        if (uq > 0.02) {
            const Arc &a = arcs[qi];
            double s0 = uq * PI / 2;
            double s1 = fmin(s0 + 14*PI/180, PI/2);
            setC(1, 0.96, 0.66, 0.45);
            glLineWidth(5.0f);
            strokeArc(a.c, a.r, a.th0 - s0, s1 - s0, 10);
        }
    }

    // comet heads
    for (int j = 0; j < NS; j++) {
        const Arc &a = arcs[j];
        double st = arcStart(j);
        double u = clamp01((t - st) / ARC_DUR);
        if (u <= 0) continue;
        double th = a.th0 - u * PI / 2;
        V hp = a.c + a.r * V{cos(th), sin(th)};
        if (u < 1) {
            drawDisc(hp, 9.0/sc, 1, 0.92, 0.45, 0.28f);
            drawDisc(hp, 5.0/sc, 1, 0.96, 0.62, 1.0f);
        } else if (t - st - ARC_DUR < 0.4) {
            double f = 1 - (t - st - ARC_DUR) / 0.4;
            drawDisc(hp, 9.0/sc, 1, 0.92, 0.45, (float)(0.3 * f));
        }
    }

    // outer golden rectangle, drawn on during the intro
    if (rectWalk > 0) {
        double al = 0.55 * (0.35 + 0.65 * clamp01((t - 0.4) / 1.2));
        setC(0.87, 0.70, 0.36, (float)al);
        glLineWidth(2.0f);
        rectWalkPath(wx0, wy0, wx1, wy1, rectWalk);
    }

    // ---- pixel pass (labels + panels)
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glOrtho(0, W, 0, H, -1, 1);
    glMatrixMode(GL_MODELVIEW);  glLoadIdentity();

    for (int j = 0; j < NS; j++) {
        const Arc &a = arcs[j];
        const Sq  &q = squares[a.sq];
        double ap = easeOut(clamp01((t - (arcStart(j) - 0.38)) / 0.30));
        double spx = q.s * sc;
        if (ap < 0.6 || spx < 13) continue;
        V ctr{(q.x0 + q.x1)/2, (q.y0 + q.y1)/2};
        V diag = ctr * 2 - a.c;                      // corner opposite arc center
        V lp = diag + (ctr - diag) * 0.35;
        V pp = w2p(lp);
        char lbl[16];
        snprintf(lbl, sizeof lbl, "%d", (int)q.s);
        double size = fmin(34, fmax(11, 0.36 * spx));
        // keep the label box fully inside its square (4 px inset)
        V sA = w2p({q.x0, q.y1}), sB = w2p({q.x1, q.y0});  // top-left, bottom-right
        double hw = textW(lbl, size) / 2 + 4, hh = size * 0.45 + 4;
        pp.x = fmin(sB.x - hw, fmax(sA.x + hw, pp.x));
        pp.y = fmin(sA.y - hh, fmax(sB.y + hh, pp.y));
        drawText(lbl, pp.x, pp.y - size*0.45, size,
                 0.93f, 0.76f, 0.42f, (float)(0.55 * ap), 1);
    }

    bool panels = marginL >= 250;
    double k = fmin(1.0, fmin(W / 1280.0, H / 800.0));   // overlay scale
    double pa = easeOut(clamp01((t - 1.15) / 0.6));
    if (panels && pa > 0) {
        float x = 40.0f * k;
        drawText("& = 1.6180339887...", x, H - 84*k, 27*k,
                 0.96f, 0.80f, 0.38f, (float)(0.92 * pa));
        double ra = easeOut(clamp01((t - 1.8) / 0.6));
        if (ra > 0) {
            drawText("F(n+1) / F(n) > &", x, H - 128*k, 15*k,
                     0.62f, 0.52f, 0.32f, (float)ra);
            // ratio events: arc for square k finishes -> F(k)/F(k-1)
            std::vector<std::pair<int,double>> ev;
            for (int sq = 2; sq <= NS; sq++) {
                int j = (sq == 2) ? 0 : sq - 1;
                if (t >= arcEnd(j)) ev.push_back({sq, FIB[sq] / FIB[sq-1]});
            }
            if (!ev.empty()) {
                char buf[64];
                snprintf(buf, sizeof buf, "%.6f", ev.back().second);
                drawText(buf, x, H - 166*k, 33*k, 0.98f, 0.84f, 0.42f, (float)ra);
                int n = (int)ev.size();
                for (int i = 1; i <= 4 && n - 1 - i >= 0; i++) {
                    int sq = ev[n - 1 - i].first;
                    snprintf(buf, sizeof buf, "F(%d)/F(%d) = %.6f",
                             sq, sq - 1, ev[n - 1 - i].second);
                    drawText(buf, x, H - (200 + 24*i)*k, 15*k,
                             0.62f, 0.52f, 0.32f, (float)(0.8 * ra));
                }
                if (holding) {
                    double fa = easeOut(clamp01((holdT - 0.9) / 0.7));
                    drawText("> & = 1.618034", x, H - 330*k, 21*k,
                             1.0f, 0.90f, 0.52f, (float)fa);
                }
            }
        }
        drawText("& = (1 + V5) / 2", W - 40.0f*k, H - 84*k, 22*k,
                 0.80f, 0.66f, 0.38f, (float)(0.85 * pa), 2);
    }

    double mb = (H - (wy1 - wy0)*sc) / 2;
    if (mb >= 56) {
        double a = easeOut(clamp01((t - 2.2) / 0.8));
        drawText("F(n): 1 * 1 * 2 * 3 * 5 * 8 * 13 * 21 * 34 * 55 * 89",
                 W / 2, 22*k, 13*k, 0.58f, 0.48f, 0.30f, (float)(0.9 * a), 1);
    }
    if (gPaused) {                 // || pause icon (font has no letters for "paused")
        glColor4f(0.85f, 0.75f, 0.50f, 0.95f * gFade);
        glLineWidth(5.0f * (float)k);
        glBegin(GL_LINES);
        glVertex2d(W/2 - 9, 46*k + 10); glVertex2d(W/2 - 9, 66*k + 10);
        glVertex2d(W/2 + 9, 46*k + 10); glVertex2d(W/2 + 9, 66*k + 10);
        glEnd();
    }
}

// ------------------------------------------------------------------ dump
static void savePPM(const char *path, int w, int h)
{
    std::vector<unsigned char> buf((size_t)w * h * 3);
    glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, buf.data());
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", path); return; }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = h - 1; y >= 0; y--) fwrite(&buf[(size_t)y*w*3], 1, (size_t)w*3, f);
    fclose(f);
}

// ------------------------------------------------------------------ main
int main(int argc, char **argv)
{
    bool dump = false;
    int dn = 0, dw = 1280, dh = 800;
    double ds = 1.0;
    const char *ddir = "/tmp/golden_frames";
    if (argc >= 3 && !strcmp(argv[1], "--dump")) {
        dump = true;
        dn = atoi(argv[2]);
        if (argc >= 4) ds = atof(argv[3]);
        if (argc >= 5) ddir = argv[4];
        if (argc >= 6) dw = atoi(argv[5]);
        if (argc >= 7) dh = atoi(argv[6]);
        if (dw < 16) dw = 16;
        if (dh < 16) dh = 16;
    }

    build();
    initFont();

    if (!glfwInit()) { fprintf(stderr, "glfwInit failed\n"); return 1; }
    glfwWindowHint(GLFW_SAMPLES, 8);   // default context on desktop GL is compatibility
    if (dump) glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);

    GLFWwindow *w = glfwCreateWindow(1280, 800, "Golden Ratio - phi", NULL, NULL);
    if (!w) { fprintf(stderr, "window creation failed\n"); glfwTerminate(); return 1; }
    glfwMakeContextCurrent(w);
    glfwSwapInterval(dump ? 0 : 1);

    if (dump) {
        mkdir(ddir, 0755);
        for (int i = 0; i < dn; i++) {
            double t = (i + 0.5) * ds;
            if (t >= LOOP) t = fmod(t, LOOP);
            render(t, dw, dh);
            char path[512];
            snprintf(path, sizeof path, "%s/f%04d.ppm", ddir, i);
            savePPM(path, dw, dh);
            printf("frame %d/%d  t=%.2f\n", i + 1, dn, t);
            fflush(stdout);
        }
        glfwDestroyWindow(w);
        glfwTerminate();
        return 0;
    }

    double last = glfwGetTime();
    double animT = 0, speed = 1;
    int prevSpace = GLFW_RELEASE, prevR = GLFW_RELEASE;
    int prevUp = GLFW_RELEASE, prevDown = GLFW_RELEASE;

    while (!glfwWindowShouldClose(w)) {
        glfwPollEvents();
        if (glfwGetKey(w, GLFW_KEY_ESCAPE) == GLFW_PRESS) break;

        int sp = glfwGetKey(w, GLFW_KEY_SPACE);
        if (sp == GLFW_PRESS && prevSpace == GLFW_RELEASE) gPaused = !gPaused;
        prevSpace = sp;

        int rr = glfwGetKey(w, GLFW_KEY_R);
        if (rr == GLFW_PRESS && prevR == GLFW_RELEASE) animT = 0;
        prevR = rr;

        int up = glfwGetKey(w, GLFW_KEY_UP);
        if (up == GLFW_PRESS && prevUp == GLFW_RELEASE) speed = fmin(4, speed * 1.25);
        prevUp = up;

        int dwn = glfwGetKey(w, GLFW_KEY_DOWN);
        if (dwn == GLFW_PRESS && prevDown == GLFW_RELEASE) speed = fmax(0.25, speed / 1.25);
        prevDown = dwn;

        double now = glfwGetTime();
        double dt = fmin(now - last, 0.1);
        last = now;
        if (!gPaused) animT = fmod(animT + dt * speed, LOOP);

        int W, H;
        glfwGetFramebufferSize(w, &W, &H);
        render(animT, W, H);
        glfwSwapBuffers(w);
    }

    glfwDestroyWindow(w);
    glfwTerminate();
    return 0;
}