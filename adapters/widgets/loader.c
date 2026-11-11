#include <stddef.h>
#include <stdint.h>
extern void *malloc(size_t);
extern void free(void *);
#ifdef HOST_TEST
void parse_original(void *o, const char *s, size_t n) {
  (void)o;
  (void)s;
  (void)n;
}
#else
extern void parse_original(void *, const char *, size_t);
#endif
#ifdef HOST_TEST
#define H
#define A
#define T
#define D
#else
#define D __attribute__((section(".xml_data")))
#define H __attribute__((section(".xml_helpers"), noinline))
#define A __attribute__((section(".xml_attrs"), noinline))
#define T __attribute__((section(".xml_transform"), noinline))
#endif
typedef struct {
  const char *p, *e;
} S;
static H int same(S s, const char *b) {
  const char *p = s.p;
  while (p < s.e && *b && *p == *b) {
    p++;
    b++;
  }
  return p == s.e && !*b;
}
static H int space(char c) {
  return c == ' ' || c == '\n' || c == '\r' || c == '\t';
}
static A const char *end_tag(const char *p, const char *e) {
  const char *term = 0;
  if (e - p >= 4 && p[0] == '<' && p[1] == '!') {
    if (p[2] == '-' && p[3] == '-')
      term = "-->";
    else if (p[2] == '[')
      term = "]]>";
  }
  if (term) {
    for (const char *q = p + 4; q + 2 < e; q++)
      if (q[0] == term[0] && q[1] == term[1] && q[2] == term[2])
        return q + 3;
    return e;
  }
  char quote = 0;
  for (; p < e; p++) {
    char c = *p;
    if (quote) {
      if (c == quote)
        quote = 0;
    } else if (c == '\'' || c == '"')
      quote = c;
    else if (c == '>')
      return p + 1;
  }
  return e;
}
static H S name(const char *p, const char *e) {
  p++;
  if (*p == '/')
    p++;
  const char *b = p;
  while (p < e && !space(*p) && *p != '>' && *p != '/')
    p++;
  return (S){b, p};
}
static H S attr(const char *p, const char *e, const char *key) {
  p = name(p, e).e;
  while (p < e) {
    while (p < e && space(*p))
      p++;
    const char *b = p;
    while (p < e && !space(*p) && *p != '=' && *p != '>' && *p != '/')
      p++;
    S n = {b, p};
    while (p < e && space(*p))
      p++;
    if (p >= e || *p != '=')
      break;
    p++;
    while (p < e && space(*p))
      p++;
    char q = *p++;
    b = p;
    while (p < e && *p != q)
      p++;
    S v = {b, p};
    if (same(n, key))
      return v;
    if (p < e)
      p++;
  }
  return (S){e, e};
}
static H char *copy(char *d, const char *b, const char *e) {
  while (b < e)
    *d++ = *b++;
  return d;
}
static H char *str(char *d, const char *s) {
  while (*s)
    *d++ = *s++;
  return d;
}
static A double expression(S s, S doc, unsigned depth);
static A double atom(S *s, S doc, unsigned depth) {
  while (s->p < s->e && space(*s->p))
    s->p++;
  if (depth > 12 || s->p == s->e)
    return -1e12;
  char c = *s->p++;
  if (c == '(') {
    double v = expression((S){s->p, s->e}, doc, depth + 1);
    int level = 1;
    while (s->p < s->e && level) {
      c = *s->p++;
      if (c == '(')
        level++;
      if (c == ')')
        level--;
    }
    return v;
  }
  if (c == '#') {
    const char *b = s->p;
    while (s->p < s->e &&
           ((*s->p >= 'a' && *s->p <= 'z') || (*s->p >= 'A' && *s->p <= 'Z') ||
            (*s->p >= '0' && *s->p <= '9') || *s->p == '_'))
      s->p++;
    S n = {b, s->p};
    if (same(n, "view_width") || same(n, "view_height"))
      return 1000;
    for (const char *p = doc.p; p < doc.e;) {
      if (*p++ != '<')
        continue;
      const char *t = p - 1, *e = end_tag(t, doc.e);
      if (same(name(t, e), "Var")) {
        S a = attr(t, e, "name");
        if (a.e - a.p == n.e - n.p) {
          const char *x = a.p, *y = n.p;
          while (x < a.e && *x == *y) {
            x++;
            y++;
          }
          if (x == a.e)
            return expression(attr(t, e, "expression"), doc, depth + 1);
        }
      }
      p = e;
    }
    return -1e12;
  }
  double v = 0, scale = 1;
  int dot = 0, any = 0;
  if (c == '-' || c == '+') {
    double v = atom(s, doc, depth + 1);
    return c == '-' ? -v : v;
  }
  s->p--;
  while (s->p < s->e) {
    c = *s->p;
    if (c == '.' && !dot) {
      dot = 1;
      s->p++;
      continue;
    }
    if (c < '0' || c > '9')
      break;
    any = 1;
    v = v * 10 + c - '0';
    if (dot)
      scale *= 10;
    s->p++;
  }
  return any ? v / scale : -1e12;
}
static A double product(S *s, S doc, unsigned depth) {
  double v = atom(s, doc, depth);
  for (;;) {
    while (s->p < s->e && space(*s->p))
      s->p++;
    if (s->p == s->e || (*s->p != '*' && *s->p != '/'))
      return v;
    char c = *s->p++;
    double q = atom(s, doc, depth);
    if (q < -1e11 || !q)
      return -1e12;
    v = c == '*' ? v * q : v / q;
  }
}
static A double expression(S s, S doc, unsigned depth) {
  double v = product(&s, doc, depth);
  for (;;) {
    while (s.p < s.e && space(*s.p))
      s.p++;
    if (s.p == s.e || *s.p == ')')
      return v;
    if (*s.p != '+' && *s.p != '-')
      return -1e12;
    char c = *s.p++;
    double q = product(&s, doc, depth);
    v = c == '+' ? v + q : v - q;
  }
}
static H int full(const char *p, const char *e, S doc) {
  S w = attr(p, e, "w"), h = attr(p, e, "h");
  if (w.p == w.e)
    w = attr(p, e, "width");
  if (h.p == h.e)
    h = attr(p, e, "height");
  return expression(w, doc, 0) >= 950 && expression(h, doc, 0) >= 950;
}
static H int visual(S n) {
  return same(n, "Group") || same(n, "Rectangle") || same(n, "Image") ||
         same(n, "Text") || same(n, "DateTime") || same(n, "Button") ||
         same(n, "Layer");
}
static const char material[] D =
    "<MaterialToken tokenName=\"Glass_Stroke_Regular\" "
    "version=\"30\"><GlassConfig token=\"Glass_Bionic_Medium_Normal\" "
    "disableGlass=\"0\" rustPassWindow=\"true\"/><BloomStrokeConfig "
    "disableBloomStroke=\"0\" bloomStrokeWidth=\"#view_width\" "
    "bloomStrokeHeight=\"#view_height\" "
    "bloomStrokeRadius=\"ifelse(#widget_radius,#widget_radius/"
    "#__scale_factor,60)\" uGeoPxPosX=\"0\" uGeoPxPosY=\"0\" "
    "uShapeEdgePow=\"3\" uColorPow=\"1.2\" uColorAdd=\"0\" uAlpha=\"1\" "
    "uShapeEdgePx=\"24\" uDarker=\"0\" uDarkerRangeX=\"0.4\" "
    "uDarkerRangeY=\"0.5\" uScaleX=\"1\" uScaleY=\"1\" uRotationTrigX=\"1\" "
    "uRotationTrigY=\"0\" uDirectionalLightDirectionX=\"-0.6\" "
    "uDirectionalLightDirectionY=\"0.7\" uDirectionalLightDirectionZ=\"-0.05\" "
    "uDirectionalLightIntensity=\"1.8\" "
    "uDirectionalLightOppositeIntensity=\"1.2\" "
    "uDirectionalLightAngleRange=\"0.7\" uDirectionalLightMinLight=\"-0.5\" "
    "uDirectionalLightLightWidth=\"8\"/></MaterialToken><ViewOutlineProvider "
    "type=\"rect\" left=\"0\" top=\"0\" right=\"#view_width\" "
    "bottom=\"#view_height\" "
    "radius=\"ifelse(#widget_radius,#widget_radius/#__scale_factor,60)\"/>";
static const char layer[] D = "<Layer frameRate=\"-1\" w=\"#view_width\" "
                              "h=\"#view_height\" layerType=\"bottom\">";
/* Package artwork that fills the card is its background plate. Images with a
 * source type load provider content (photos, covers); they stay visible and
 * translucent over the glass, keeping their own alpha animation. */
static T int external(const char *p, const char *e) {
  S t = attr(p, e, "srcType");
  return t.p != t.e;
}
static H char *tag(char *d, const char *p, const char *e, S doc, int root) {
  S n = name(p, e);
  int text = same(n, "Text") || same(n, "DateTime"),
      rect = same(n, "Rectangle"), picture = same(n, "Image"),
      photo = picture && external(p, e),
      image = (picture && !photo &&
               (full(p, e, doc) ||
                (attr(p, e, "centerX").p != attr(p, e, "centerX").e &&
                 expression(attr(p, e, "h"), doc, 0) >= 950))) ||
              (rect && full(p, e, doc));
  S alpha = attr(p, e, "alpha");
  d = copy(d, p, n.e);
  p = n.e;
  while (p < e) {
    const char *b = p;
    while (p < e && space(*p))
      p++;
    const char *k = p;
    while (p < e && !space(*p) && *p != '=' && *p != '>' && *p != '/')
      p++;
    S a = {k, p};
    while (p < e && space(*p))
      p++;
    if (p >= e || *p != '=')
      break;
    p++;
    while (p < e && space(*p))
      p++;
    char q = *p++;
    while (p < e && *p != q)
      p++;
    if (p < e)
      p++;
    int omit =
        (text && same(a, "color")) || (rect && same(a, "fillColor")) ||
        (image && same(a, "visibility")) || (photo && same(a, "alpha")) ||
        (root && (same(a, "version") || same(a, "supportBackgroundBlur") ||
                  same(a, "disableCutRoundCorner") ||
                  same(a, "customizedDarkModeWallpaper") ||
                  same(a, "specialShapeWidget") || same(a, "stackWidget")));
    if (!omit)
      d = copy(d, b, p);
  }
  if (text)
    d = str(d, " color=\"#EFFFFFFF\"");
  if (rect)
    d = str(d, " fillColor=\"#12FFFFFF\"");
  if (image)
    d = str(d, " visibility=\"0\"");
  if (photo) {
    d = str(d, " alpha=\"(");
    d = alpha.p == alpha.e ? str(d, "255") : copy(d, alpha.p, alpha.e);
    d = str(d, ")*0.6\"");
  }
  if (root)
    d = str(d, " version=\"2\" supportBackgroundBlur=\"true\" "
               "disableCutRoundCorner=\"false\" specialShapeWidget=\"0\" "
               "stackWidget=\"0\"");
  if (e - p >= 2 && e[-2] == '/')
    *d++ = '/';
  *d++ = '>';
  return d;
}
/* Called at the common native DOM parser. Input is borrowed, output is owned
 * until parse_original returns. Root metadata and provider/action subtrees stay
 * at the root; only visual subtrees move into the common material layer. */
T char *style_xml(const char *src, size_t len, size_t *out) {
  if (len > 2000000)
    return 0;
  S doc = {src, src + len};
  int card = 0, existing = 0;
  for (const char *p = src; p < doc.e;) {
    if (*p != '<') {
      p++;
      continue;
    }
    const char *e = end_tag(p, doc.e);
    S n = name(p, e);
    if ((same(n, "Rectangle") || same(n, "Image")) && p[1] != '/' &&
        full(p, e, doc) && !same(attr(p, e, "visibility"), "0"))
      card = 1;
    if (same(n, "Widget") && p[1] != '/')
      existing = 1;
    p = e;
  }
  if (!card || !existing)
    return 0;
  char *mem = malloc(12 * len + 8192);
  if (!mem)
    return 0;
  char *d = mem, *fg = mem + 6 * len + 4096, *f = fg;
  unsigned depth = 0, vdepth = 0, skip = 0;
  int in_layer = 0;
  for (const char *p = src; p < doc.e;) {
    if (*p != '<') {
      if (!skip) {
        if (vdepth)
          *f++ = *p;
        else
          *d++ = *p;
      }
      p++;
      continue;
    }
    const char *e = end_tag(p, doc.e);
    S n = name(p, e);
    int close = p[1] == '/', self = e - p >= 2 && e[-2] == '/',
        special = p[1] == '!' || p[1] == '?';
    if (special) {
      if (p[1] == '!' && p + 3 < doc.e && p[2] == '-' && p[3] == '-') {
        e = p + 4;
        while (e + 2 < doc.e && !(e[0] == '-' && e[1] == '-' && e[2] == '>'))
          e++;
        if (e + 2 < doc.e)
          e += 3;
      }
      if (!skip) {
        if (vdepth)
          f = copy(f, p, e);
        else
          d = copy(d, p, e);
      }
      p = e;
      continue;
    }
    if (close) {
      if (depth)
        depth--;
      if (skip) {
        if (depth == skip)
          skip = 0;
        p = e;
        continue;
      }
      if (depth == 0 && same(n, "Widget")) {
        d = str(d, layer);
        d = str(d, material);
        d = copy(d, fg, f);
        d = str(d, "</Layer>");
      }
      if (vdepth) {
        if (in_layer && same(n, "Layer") && depth == vdepth - 1) {
          f = str(f, "</Group>");
          in_layer = 0;
        } else
          f = copy(f, p, e);
        if (depth == vdepth - 1)
          vdepth = 0;
      } else
        d = copy(d, p, e);
      p = e;
      continue;
    }
    if (skip) {
      if (!self)
        depth++;
      p = e;
      continue;
    }
    if (depth == 1 && visual(n))
      vdepth = depth + 1;
    if (vdepth &&
        (same(n, "MaterialToken") || same(n, "ViewOutlineProvider"))) {
      if (!self) {
        skip = depth;
        depth++;
      }
      p = e;
      continue;
    }
    if (vdepth) {
      if (depth == 1 && same(n, "Layer")) {
        f = str(f, "<Group>");
        in_layer = 1;
      } else
        f = tag(f, p, e, doc, 0);
    } else
      d = tag(d, p, e, doc, depth == 0 && same(n, "Widget"));
    if (!self)
      depth++;
    else if (vdepth == depth + 1)
      vdepth = 0;
    p = e;
  }
  *out = (size_t)(d - mem);
  return mem;
}
static T long system_call(long nr, long a, long b, long c) {
#ifdef HOST_TEST
  (void)nr;
  (void)a;
  (void)b;
  (void)c;
  return -1;
#else
  register long x0 __asm__("x0") = a, x1 __asm__("x1") = b,
                   x2 __asm__("x2") = c, x3 __asm__("x3") = 0,
                   x8 __asm__("x8") = nr;
  __asm__ volatile("svc #0"
                   : "+r"(x0)
                   : "r"(x1), "r"(x2), "r"(x3), "r"(x8)
                   : "memory");
  return x0;
#endif
}
static H int is_launcher(void) {
  char cmd[32];
  long fd = system_call(56, -100, (long)"/proc/self/cmdline", 0);
  if (fd < 0)
    return 0;
  long n = system_call(63, fd, (long)cmd, sizeof(cmd));
  system_call(57, fd, 0, 0);
  if (n <= 0)
    return 0;
  const char *e = cmd;
  while (e < cmd + n && *e)
    e++;
  return same((S){cmd, e}, "com.miui.home");
}
T void parse_hook(void *out, const char *src, size_t n) {
  size_t size = n;
  char *styled = is_launcher() ? style_xml(src, n, &size) : 0;
  parse_original(out, styled ? styled : src, size);
  if (styled)
    free(styled);
}
