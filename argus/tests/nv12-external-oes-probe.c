// Does a GL_TEXTURE_EXTERNAL_OES sampler read an NV12 NvBufSurface correctly?
//
// Independent of score: allocate an NvBufSurface NV12, fill it with a known
// pattern through the CPU mapping, import the whole thing as ONE EGLImage,
// sample it through samplerExternalOES into an FBO, read the FBO back, and
// check the pixels against what the pattern should produce.
//
// Exists because score's own grab writes black for every rung on this board, so
// it cannot tell "the external sampler is broken" from "the readback is
// broken". This asks only the first question.
//
// Build: see oes-repro-build.sh   Run on the board: ./oes-repro

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <nvbufsurface.h>

#include <stdio.h>

// Resolved at runtime: the extension entry points are not declared by the
// headers on this target, and an implicit declaration returns int, which
// truncates the EGLImage pointer.
typedef void* (*PFN_eglCreateImageKHR)(void*, void*, unsigned, void*, const int*);
typedef void (*PFN_glEGLImageTargetTexture2DOES)(unsigned, void*);
static PFN_eglCreateImageKHR p_eglCreateImageKHR;
static PFN_glEGLImageTargetTexture2DOES p_glEGLImageTargetTexture2DOES;

#ifndef DRM_FORMAT_NV12
#define DRM_FORMAT_NV12 0x3231564e
#endif
#include <stdlib.h>
#include <string.h>

#define W 640
#define H 480

static int fail(const char* what)
{
  printf("FAIL: %s (egl 0x%x, gl 0x%x)\n", what, eglGetError(), glGetError());
  return 1;
}

// Y for a known colour, and the U/V that go with it. BT.709 limited range,
// matching what the ISP emits and what an external sampler should undo.
static void rgb_to_yuv(int r, int g, int b, unsigned char* y, unsigned char* u,
                       unsigned char* v)
{
  double yf = 0.2126 * r + 0.7152 * g + 0.0722 * b;
  *y = (unsigned char)(16 + yf * 219.0 / 255.0);
  *u = (unsigned char)(128 + (b - yf) * 0.5389 * 224.0 / 255.0);
  *v = (unsigned char)(128 + (r - yf) * 0.6350 * 224.0 / 255.0);
}

static const char* VS =
    "#version 300 es\n"
    "out vec2 uv;\n"
    "void main(){\n"
    "  vec2 p = vec2((gl_VertexID<<1)&2, gl_VertexID&2);\n"
    "  uv = p; gl_Position = vec4(p*2.0-1.0, 0.0, 1.0);\n"
    "}\n";

static const char* FS =
    "#version 300 es\n"
    "#extension GL_OES_EGL_image_external_essl3 : require\n"
    "precision mediump float;\n"
    "uniform samplerExternalOES tex;\n"
    "in vec2 uv;\n"
    "out vec4 frag;\n"
    "void main(){ frag = texture(tex, uv); }\n";

static GLuint compile(GLenum type, const char* src)
{
  GLuint s = glCreateShader(type);
  glShaderSource(s, 1, &src, NULL);
  glCompileShader(s);
  GLint ok = 0;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if(!ok)
  {
    char log[4096];
    glGetShaderInfoLog(s, sizeof log, NULL, log);
    printf("shader compile failed:\n%s\n", log);
    return 0;
  }
  return s;
}

int main(void)
{
  EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if(dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, NULL, NULL))
    return fail("eglInitialize");

  const char* exts = eglQueryString(dpy, EGL_EXTENSIONS);
  printf("EGL_EXT_image_dma_buf_import: %s\n",
         strstr(exts ? exts : "", "EGL_EXT_image_dma_buf_import") ? "yes" : "NO");

  eglBindAPI(EGL_OPENGL_ES_API);
  EGLConfig cfg;
  EGLint n = 0;
  const EGLint cfgAttr[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
                            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
                            EGL_NONE};
  if(!eglChooseConfig(dpy, cfgAttr, &cfg, 1, &n) || n < 1)
    return fail("eglChooseConfig");

  const EGLint ctxAttr[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
  EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctxAttr);
  if(ctx == EGL_NO_CONTEXT)
    return fail("eglCreateContext");
  const EGLint pbAttr[] = {EGL_WIDTH, W, EGL_HEIGHT, H, EGL_NONE};
  EGLSurface surf = eglCreatePbufferSurface(dpy, cfg, pbAttr);
  if(!eglMakeCurrent(dpy, surf, surf, ctx))
    return fail("eglMakeCurrent");

  printf("GL_RENDERER: %s\n", glGetString(GL_RENDERER));
  const char* gl_exts = (const char*)glGetString(GL_EXTENSIONS);
  printf("GL_OES_EGL_image_external_essl3: %s\n",
         strstr(gl_exts ? gl_exts : "", "GL_OES_EGL_image_external_essl3") ? "yes"
                                                                          : "NO");

  // --- allocate an NV12 surface the way ArgusSession does -------------------
  NvBufSurfaceAllocateParams p;
  memset(&p, 0, sizeof p);
  p.params.width = W;
  p.params.height = H;
  p.params.colorFormat = NVBUF_COLOR_FORMAT_NV12;
  p.params.layout = NVBUF_LAYOUT_PITCH;
  p.params.memType = NVBUF_MEM_SURFACE_ARRAY;
  p.memtag = NvBufSurfaceTag_CAMERA;
  p.disablePitchPadding = true;

  NvBufSurface* s = NULL;
  if(NvBufSurfaceAllocate(&s, 1, &p) != 0 || !s)
    return fail("NvBufSurfaceAllocate");
  s->numFilled = 1;
  NvBufSurfaceParams* sp = &s->surfaceList[0];
  printf("surface: %ux%u planes=%u p0(off=%u pitch=%u) p1(off=%u pitch=%u) fd=%d\n",
         sp->width, sp->height, sp->planeParams.num_planes,
         sp->planeParams.offset[0], sp->planeParams.pitch[0],
         sp->planeParams.offset[1], sp->planeParams.pitch[1],
         (int)sp->bufferDesc);

  // --- fill with a known pattern: left half red, right half blue ------------
  if(NvBufSurfaceMap(s, 0, -1, NVBUF_MAP_READ_WRITE) != 0)
    return fail("NvBufSurfaceMap");
  unsigned char* yp = (unsigned char*)sp->mappedAddr.addr[0];
  unsigned char* uvp = (unsigned char*)sp->mappedAddr.addr[1];
  if(!yp || !uvp)
    return fail("mapped plane address is null");

  unsigned char ry, ru, rv, by, bu, bv;
  rgb_to_yuv(255, 0, 0, &ry, &ru, &rv);
  rgb_to_yuv(0, 0, 255, &by, &bu, &bv);
  for(unsigned r = 0; r < sp->height; r++)
    for(unsigned c = 0; c < sp->width; c++)
      yp[r * sp->planeParams.pitch[0] + c] = (c < W / 2) ? ry : by;
  for(unsigned r = 0; r < sp->height / 2; r++)
    for(unsigned c = 0; c < sp->width / 2; c++)
    {
      unsigned char* px = &uvp[r * sp->planeParams.pitch[1] + c * 2];
      px[0] = (c < W / 4) ? ru : bu;
      px[1] = (c < W / 4) ? rv : bv;
    }
  NvBufSurfaceSyncForDevice(s, 0, -1);

  // --- import the WHOLE frame as one EGLImage -------------------------------
  const int fd = (int)sp->bufferDesc;
  EGLint attr[] = {EGL_WIDTH, (EGLint)W,
                   EGL_HEIGHT, (EGLint)H,
                   EGL_LINUX_DRM_FOURCC_EXT, (EGLint)DRM_FORMAT_NV12,
                   EGL_DMA_BUF_PLANE0_FD_EXT, fd,
                   EGL_DMA_BUF_PLANE0_OFFSET_EXT, (EGLint)sp->planeParams.offset[0],
                   EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)sp->planeParams.pitch[0],
                   EGL_DMA_BUF_PLANE1_FD_EXT, fd,
                   EGL_DMA_BUF_PLANE1_OFFSET_EXT, (EGLint)sp->planeParams.offset[1],
                   EGL_DMA_BUF_PLANE1_PITCH_EXT, (EGLint)sp->planeParams.pitch[1],
                   EGL_NONE};
  p_eglCreateImageKHR
      = (PFN_eglCreateImageKHR)eglGetProcAddress("eglCreateImageKHR");
  p_glEGLImageTargetTexture2DOES = (PFN_glEGLImageTargetTexture2DOES)
      eglGetProcAddress("glEGLImageTargetTexture2DOES");
  if(!p_eglCreateImageKHR || !p_glEGLImageTargetTexture2DOES)
    return fail("extension entry points missing");

  void* img = p_eglCreateImageKHR(dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT,
                                  NULL, attr);
  if(!img)
    return fail("eglCreateImageKHR(NV12, 2 planes)");
  printf("eglCreateImageKHR: ok\n");

  GLuint tex = 0;
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_EXTERNAL_OES, tex);
  glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  while(glGetError())
    ;
  p_glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, img);
  if(glGetError() != GL_NO_ERROR)
    return fail("glEGLImageTargetTexture2DOES");
  printf("glEGLImageTargetTexture2DOES: ok\n");

  // --- sample it into an FBO ------------------------------------------------
  GLuint prog = glCreateProgram();
  GLuint vs = compile(GL_VERTEX_SHADER, VS), fs = compile(GL_FRAGMENT_SHADER, FS);
  if(!vs || !fs)
    return 1;
  glAttachShader(prog, vs);
  glAttachShader(prog, fs);
  glLinkProgram(prog);
  GLint ok = 0;
  glGetProgramiv(prog, GL_LINK_STATUS, &ok);
  if(!ok)
  {
    char log[4096];
    glGetProgramInfoLog(prog, sizeof log, NULL, log);
    printf("link failed:\n%s\n", log);
    return 1;
  }

  GLuint rt = 0, fbo = 0;
  glGenTextures(1, &rt);
  glBindTexture(GL_TEXTURE_2D, rt);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  glGenFramebuffers(1, &fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt, 0);
  if(glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
    return fail("FBO incomplete");

  GLuint vao = 0;
  glGenVertexArrays(1, &vao);
  glBindVertexArray(vao);
  glViewport(0, 0, W, H);
  glClearColor(0, 0, 0, 1);
  glClear(GL_COLOR_BUFFER_BIT);
  glUseProgram(prog);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_EXTERNAL_OES, tex);
  glUniform1i(glGetUniformLocation(prog, "tex"), 0);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  if(glGetError() != GL_NO_ERROR)
    return fail("draw");

  unsigned char* out = malloc(W * H * 4);
  glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, out);
  if(glGetError() != GL_NO_ERROR)
    return fail("glReadPixels");

  // --- verdict --------------------------------------------------------------
  // Sample well inside each half, away from the chroma edge.
  unsigned char* L = &out[((H / 2) * W + W / 4) * 4];
  unsigned char* R = &out[((H / 2) * W + (3 * W) / 4) * 4];
  printf("left  pixel: R=%3d G=%3d B=%3d  (expect red)\n", L[0], L[1], L[2]);
  printf("right pixel: R=%3d G=%3d B=%3d  (expect blue)\n", R[0], R[1], R[2]);

  long sum = 0;
  for(int i = 0; i < W * H * 4; i += 4)
    sum += out[i] + out[i + 1] + out[i + 2];
  printf("mean luminance: %.1f\n", sum / (double)(W * H * 3));

  const int leftRed = L[0] > 128 && L[2] < 96;
  const int rightBlue = R[2] > 128 && R[0] < 96;
  printf("%s\n", (leftRed && rightBlue)
                     ? "PASS: external sampler returns the expected colours"
                     : "FAIL: external sampler did not return the pattern");
  free(out);
  return (leftRed && rightBlue) ? 0 : 1;
}
