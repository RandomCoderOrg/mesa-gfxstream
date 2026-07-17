/*
 * Minimal GLX BC3/DXT5 sampling probe.
 *
 * Uploads one opaque-red 4x4 BC3 block, draws it over the window, and reads
 * the centre pixel back. A working implementation prints approximately
 * "pixel: 255 0 0 255".
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <X11/Xlib.h>
#include <GL/gl.h>
#include <GL/glx.h>

#ifndef GL_COMPRESSED_RGBA_S3TC_DXT5_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83F3
#endif

typedef void (*compressed_tex_image_2d_fn)(GLenum, GLint, GLenum, GLsizei,
                                           GLsizei, GLint, GLsizei,
                                           const void *);

static void
die(const char *message)
{
        fprintf(stderr, "%s\n", message);
        exit(EXIT_FAILURE);
}

int
main(void)
{
        static const int visual_attribs[] = {
                GLX_RGBA, GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8,
                GLX_BLUE_SIZE, 8, GLX_DOUBLEBUFFER, None,
        };
        /* BC3: opaque alpha followed by two identical RGB565 red endpoints. */
        static const unsigned char red_bc3[16] = {
                0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0xf8, 0x00, 0xf8, 0x00, 0x00, 0x00, 0x00,
        };
        Display *display = XOpenDisplay(NULL);
        XVisualInfo *visual;
        Colormap colormap;
        XSetWindowAttributes swa;
        Window window;
        GLXContext context;
        compressed_tex_image_2d_fn compressed_tex_image_2d;
        GLuint texture;
        unsigned char pixel[4] = {0};
        GLenum error;

        if (!display)
                die("XOpenDisplay failed");

        visual = glXChooseVisual(display, DefaultScreen(display),
                                 (int *)visual_attribs);
        if (!visual)
                die("glXChooseVisual failed");

        colormap = XCreateColormap(display, RootWindow(display, visual->screen),
                                   visual->visual, AllocNone);
        memset(&swa, 0, sizeof(swa));
        swa.colormap = colormap;
        swa.event_mask = StructureNotifyMask;
        window = XCreateWindow(display, RootWindow(display, visual->screen),
                               0, 0, 128, 128, 0, visual->depth, InputOutput,
                               visual->visual, CWColormap | CWEventMask, &swa);
        XStoreName(display, window, "Panfrost BC3 probe");
        XMapWindow(display, window);

        context = glXCreateContext(display, visual, NULL, True);
        if (!context || !glXMakeCurrent(display, window, context))
                die("GLX context setup failed");

        compressed_tex_image_2d = (compressed_tex_image_2d_fn)
                glXGetProcAddressARB((const GLubyte *)"glCompressedTexImage2D");
        if (!compressed_tex_image_2d)
                die("glCompressedTexImage2D unavailable");

        glViewport(0, 0, 128, 128);
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();

        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        compressed_tex_image_2d(GL_TEXTURE_2D, 0,
                                GL_COMPRESSED_RGBA_S3TC_DXT5_EXT,
                                4, 4, 0, sizeof(red_bc3), red_bc3);
        error = glGetError();
        printf("upload error: 0x%04x\n", error);

        glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glEnable(GL_TEXTURE_2D);
        glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
        glBegin(GL_TRIANGLE_STRIP);
        glTexCoord2f(0.0f, 0.0f); glVertex2f(-1.0f, -1.0f);
        glTexCoord2f(1.0f, 0.0f); glVertex2f( 1.0f, -1.0f);
        glTexCoord2f(0.0f, 1.0f); glVertex2f(-1.0f,  1.0f);
        glTexCoord2f(1.0f, 1.0f); glVertex2f( 1.0f,  1.0f);
        glEnd();
        glFinish();

        glReadPixels(64, 64, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
        error = glGetError();
        printf("read error: 0x%04x\n", error);
        printf("pixel: %u %u %u %u\n", pixel[0], pixel[1], pixel[2], pixel[3]);

        glXSwapBuffers(display, window);
        glDeleteTextures(1, &texture);
        glXMakeCurrent(display, None, NULL);
        glXDestroyContext(display, context);
        XDestroyWindow(display, window);
        XFreeColormap(display, colormap);
        XFree(visual);
        XCloseDisplay(display);

        return (pixel[0] > 240 && pixel[1] < 16 && pixel[2] < 16 &&
                pixel[3] > 240) ? EXIT_SUCCESS : EXIT_FAILURE;
}
