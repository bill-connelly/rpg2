#include <Python.h>
#include <pthread.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <gbm.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/time.h>
#include <math.h>


#define PI 3.141
#define PARAMETER_LENGTH 7

//gcc -shared -o rpg.so -fPIC rpg.c -O3 -lEGL -lGLESv2 -ldrm -lgbm -lm -I/usr/include/libdrm -I/usr/include/python3.11


unsigned int
  frameCount = 0,
  trisPerCirc = 40;

static const EGLint configAttribs[] = {
    EGL_RED_SIZE, 8,
    EGL_GREEN_SIZE, 8,
    EGL_BLUE_SIZE, 8,
    EGL_DEPTH_SIZE, 8,
    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
    EGL_NONE
};

static const EGLint contextAttribs[] = {
    EGL_CONTEXT_CLIENT_VERSION, 2,
    EGL_NONE
};


typedef struct {
    GLuint vertexShaderId;
    GLuint fragmentShaderId;
    GLuint VAOId;
    GLuint VBOId;
    GLuint programId;
    int VBOlength;
    float angle;
    float spatial;
    float cyclesPerSecond;
    float aspectRatio;
} shader;

typedef struct {
    int device;
    EGLDisplay display;
    EGLContext context;
    EGLSurface surface;
    shader* currentShaderPtr;
} GLconfig;

struct timeval tv;
long get_time_micros() {
    gettimeofday(&tv,NULL);
    return 1000000 * tv.tv_sec + tv.tv_usec;
}

// The following code related to DRM/GBM was adapted from the following sources:
// https://github.com/eyelash/tutorials/blob/master/drm-gbm.c
// and
// https://www.raspberrypi.org/forums/viewtopic.php?t=243707#p1499181
//
// I am not the original author of this code, I have only modified it.

drmModeModeInfo mode;
struct gbm_device *gbmDevice;
struct gbm_surface *gbmSurface;
drmModeCrtc *crtc;
uint32_t connectorId;

static drmModeConnector *getConnector(drmModeRes *resources, int device) {
    for (int i = 0; i < resources->count_connectors; i++)
    {
        drmModeConnector *connector = drmModeGetConnector(device, resources->connectors[i]);
        if (connector->connection == DRM_MODE_CONNECTED)
        {
            return connector;
        }
        drmModeFreeConnector(connector);
    }

    return NULL;
}

static drmModeEncoder *findEncoder(drmModeConnector *connector, int device) {
    if (connector->encoder_id) {
        return drmModeGetEncoder(device, connector->encoder_id);
    }
    return NULL;
}

static int getDisplay(EGLDisplay *display, int device) {
    drmModeRes *resources = drmModeGetResources(device);
    if (resources == NULL)  {
        fprintf(stderr, "Unable to get DRM resources\n");
        return -1;
    }

    drmModeConnector *connector = getConnector(resources, device);
    if (connector == NULL)  {
        fprintf(stderr, "Unable to get connector\n");
        drmModeFreeResources(resources);
        return -1;
    }

    connectorId = connector->connector_id;
    mode = connector->modes[0];


    // printf("count modes: %i\n", connector->count_modes);
    // drmModeModeInfo myMode;
    // for (int i = 0; i<connector->count_modes; i++) {
    //     myMode = connector->modes[i];
    //     printf("resolution: %ix%i\n", myMode.hdisplay, myMode.vdisplay);
    // } 
    
    
    
    printf("Resolution: %ix%i\n", mode.hdisplay, mode.vdisplay);

    drmModeEncoder *encoder = findEncoder(connector, device);
    if (encoder == NULL) {
        fprintf(stderr, "Unable to get encoder\n");
        drmModeFreeConnector(connector);
        drmModeFreeResources(resources);
        return -1;
    }

    crtc = drmModeGetCrtc(device, encoder->crtc_id);
    drmModeFreeEncoder(encoder);
    drmModeFreeConnector(connector);
    drmModeFreeResources(resources);
    gbmDevice = gbm_create_device(device);
    gbmSurface = gbm_surface_create(gbmDevice, mode.hdisplay, mode.vdisplay, GBM_FORMAT_XRGB8888, GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
    *display = eglGetDisplay(gbmDevice);
    return 0;
}

static int matchConfigToVisual(EGLDisplay display, EGLint visualId, EGLConfig *configs, int count) {
    EGLint id;
    for (int i = 0; i < count; ++i)    {
        if (!eglGetConfigAttrib(display, configs[i], EGL_NATIVE_VISUAL_ID, &id)) {
            continue;
        }
        if (id == visualId) {
            return i;
        }
    }
    return -1;

}

static struct gbm_bo *previousBo = NULL;
static uint32_t previousFb;

static void gbmSwapBuffers(EGLDisplay *display, EGLSurface *surface, int device) {
    eglSwapBuffers(*display, *surface);
    struct gbm_bo *bo = gbm_surface_lock_front_buffer(gbmSurface);
    uint32_t handle = gbm_bo_get_handle(bo).u32;
    uint32_t pitch = gbm_bo_get_stride(bo);
    uint32_t fb;
    drmModeAddFB(device, mode.hdisplay, mode.vdisplay, 24, 32, pitch, handle, &fb);
    drmModeSetCrtc(device, crtc->crtc_id, fb, 0, 0, &connectorId, 1, &mode);

    if (previousBo) {
        drmModeRmFB(device, previousFb);
        gbm_surface_release_buffer(gbmSurface, previousBo);
    }
    previousBo = bo;
    previousFb = fb;
}

static void gbmClean(int device) {
    // set the previous crtc
    drmModeSetCrtc(device, crtc->crtc_id, crtc->buffer_id, crtc->x, crtc->y, &connectorId, 1, &crtc->mode);
    drmModeFreeCrtc(crtc);

    if (previousBo) {
        drmModeRmFB(device, previousFb);
        gbm_surface_release_buffer(gbmSurface, previousBo);
    }

    gbm_surface_destroy(gbmSurface);
    gbm_device_destroy(gbmDevice);
}

const char* vertexShaderSource =
    "attribute vec3 pos;"
    "varying vec3 fragPos;"
    "void main() {"
    "    gl_Position = vec4(pos, 1.0);"
    "    fragPos = pos;"
    "}";

// char sinFragSourceBuffer[] = 
//     "uniform vec4 color;"
//     "uniform float time;"
//     "uniform float angle;"
//     "uniform float aspectRatio;"
//     "varying vec3 fragPos;"
//     "float phase = 0.00000;"
//     "const float spatial = 20.1000;"
//     //"const float angle = 1.07050;" 
//     "float cosine = cos(angle)*spatial*aspectRatio;"
//     "float sine = sin(angle)*spatial;"
//     "void main() {"
//     " phase = time / 0.2;"
//     " float m = sin(cosine*fragPos.x + sine*fragPos.y + phase) * 0.5 + 0.5;"
//     " gl_FragColor = vec4(m, m, m, 1.0);"
//     "}";

char sinFragSourceBuffer[] = 
    "uniform float time;"
    "uniform float angle;"
    "uniform float spatial;"
    "uniform float aspectRatio;"
    "uniform float cyclesPerSecond;"
    "varying vec3 fragPos;"
    "float phase = 0.0;"

    "float cosine = cos(angle)*spatial*aspectRatio;"
    "float sine = sin(angle)*spatial;"
    "void main() {"
    " phase = 2.0 * 3.1415 * time * cyclesPerSecond;"
    " float m = sin(cosine*fragPos.x + sine*fragPos.y + phase) * 0.5 + 0.5;"
    " gl_FragColor = vec4(m, m, m, 1.0);"
    "}";

const char* sinFragSource = sinFragSourceBuffer;

char squareFragSourceBuffer[] = 
    "uniform vec4 color;"
    "uniform float time;"
    "varying vec3 fragPos;"
    "float phase = 0.00000;"
    "const float spatial = 20.1000;"
    "const float angle = 1.57050;" 
    "float cosine = cos(angle)*spatial;"
    "float sine = sin(angle)*spatial;"
    "void main() {"
    " phase = time / 0.2;"
    " float m = step(sin(cosine*fragPos.x + sine*fragPos.y + phase), 0.0);"
    " gl_FragColor = vec4(m, m, m, 1.0);"
    "}";

const char* squareFragSource = squareFragSourceBuffer;

char gaborFragSourceBuffer[] = 
    "uniform vec4 color;"
    "uniform float time;"
    "varying vec3 fragPos;"
    "float phase = 0.00000;"
    "const float spatial = 20.1000;"
    "const float angle = 1.00000;"
    "const float sigma = 0.10000;"
    "const float center_x = 0.00000;"
    "const float center_y = 0.00000;"
    "float cosine = cos(angle)*spatial;"
    "float sine = sin(angle)*spatial;"
    "float gaussian(float x, float y) {"
    "  return  exp( -((x-center_x)*(x-center_x) + (y-center_y)*(y-center_y) ) / sigma ) ;"
    "}"
    "void main() {"
    " phase = time / 0.2;"
    " float m = gaussian(fragPos.x, fragPos.y) * sin(cosine*fragPos.x + sine*fragPos.y + phase) * 0.5 + 0.5;"
    " gl_FragColor =  vec4(m, m, m, 1.0);"
    "}";

const char* gaborFragSource = gaborFragSourceBuffer;

// Get the EGL error back as a string. Useful for debugging.
static const char *eglGetErrorStr() {
    switch (eglGetError()){
    case EGL_SUCCESS:
        return "The last function succeeded without error.";
    case EGL_NOT_INITIALIZED:
        return "EGL is not initialized, or could not be initialized, for the "
               "specified EGL display connection.";
    case EGL_BAD_ACCESS:
        return "EGL cannot access a requested resource (for example a context "
               "is bound in another thread).";
    case EGL_BAD_ALLOC:
        return "EGL failed to allocate resources for the requested operation.";
    case EGL_BAD_ATTRIBUTE:
        return "An unrecognized attribute or attribute value was passed in the "
               "attribute list.";
    case EGL_BAD_CONTEXT:
        return "An EGLContext argument does not name a valid EGL rendering "
               "context.";
    case EGL_BAD_CONFIG:
        return "An EGLConfig argument does not name a valid EGL frame buffer "
               "configuration.";
    case EGL_BAD_CURRENT_SURFACE:
        return "The current surface of the calling thread is a window, pixel "
               "buffer or pixmap that is no longer valid.";
    case EGL_BAD_DISPLAY:
        return "An EGLDisplay argument does not name a valid EGL display "
               "connection.";
    case EGL_BAD_SURFACE:
        return "An EGLSurface argument does not name a valid surface (window, "
               "pixel buffer or pixmap) configured for GL rendering.";
    case EGL_BAD_MATCH:
        return "Arguments are inconsistent (for example, a valid context "
               "requires buffers not supplied by a valid surface).";
    case EGL_BAD_PARAMETER:
        return "One or more argument values are invalid.";
    case EGL_BAD_NATIVE_PIXMAP:
        return "A NativePixmapType argument does not refer to a valid native "
               "pixmap.";
    case EGL_BAD_NATIVE_WINDOW:
        return "A NativeWindowType argument does not refer to a valid native "
               "window.";
    case EGL_CONTEXT_LOST:
        return "A power management event has occurred. The application must "
               "destroy all contexts and reinitialise OpenGL ES state and "
               "objects to continue rendering.";
    default:
        break;
    }
    return "Unknown error!";
}

static const char *glGetErrorStr(GLenum errorCheckValue) {
    switch (errorCheckValue) {
        case GL_NO_ERROR:
            return "The last function succeeded without error.";
        case GL_INVALID_ENUM:
            return "An unacceptable value is specified for an enumerated argument. The offending command is ignored and has no other side effect than to set the error flag.";
        case GL_INVALID_VALUE:
            return "A numeric argument is out of range. The offending command is ignored and has no other side effect than to set the error flag.";
        case GL_INVALID_OPERATION:
            return "The specified operation is not allowed in the current state. The offending command is ignored and has no other side effect than to set the error flag.";
        case GL_INVALID_FRAMEBUFFER_OPERATION:
            return "The framebuffer object is not complete. The offending command is ignored and has no other side effect than to set the error flag.";
        case GL_OUT_OF_MEMORY:
            return "There is not enough memory left to execute the command. The state of the GL is undefined, except for the state of the error flags, after this error is recorded.";
        default:
            break;
    }
    return "Unknown error!";
}

void createCircle(GLfloat *vertices) {
    float aspectRatio = (float)mode.hdisplay / mode.vdisplay;
    float cx = 0.0;
    float cy = 0.0;
    float r = 0.5;
    float delTheta = PI * 2 / trisPerCirc;

    float x = r;
    float y = 0;

    for (int i = 0; i < trisPerCirc; i++) {
        vertices[i*9+0] = x + cx;
        vertices[i*9+1] = y*aspectRatio + cy;
        vertices[i*9+2] = 0.0f;

        x = (float)r*cosf(delTheta*(i+1));
        y = (float)r*sinf(delTheta*(i+1));

        vertices[i*9+3] = x + cx;
        vertices[i*9+4] = y*aspectRatio + cy;
        vertices[i*9+5] = 0.0f;

        vertices[i*9+6] = cx;
        vertices[i*9+7] = cy;
        vertices[i*9+8] = 0.0f;
    }

    vertices[(trisPerCirc-1)*9+3] = vertices[0];
    vertices[(trisPerCirc-1)*9+4] = vertices[1];
    vertices[(trisPerCirc-1)*9+5] = vertices[2];
}

void createRect(GLfloat *vertices) {

    float top = 1.0;
    float right = 1.0;
    float left = -1.0;
    float bottom = -1.0;
    float rect[] = {left,  top,    0.0,
                    right, top,    0.0,
                    left,  bottom, 0.0,
                    left,  bottom, 0.0,
                    right, top,    0.0,
                    right, bottom, 0.0};
    
    memcpy(vertices, rect, sizeof(rect));   
}

void createVBO(shader* shaderPtr) {

    int shape = 0;

    GLfloat *vertices;

    if (shape) {
        shaderPtr->VBOlength = trisPerCirc * 3 * 3;
        vertices = (GLfloat *)malloc(shaderPtr->VBOlength * sizeof(GLfloat));
        createCircle(vertices);
    } else {
        shaderPtr->VBOlength = 2 * 3 * 3;
        vertices = (GLfloat *)malloc(shaderPtr->VBOlength * sizeof(GLfloat));
        createRect(vertices);
    }

    if (vertices == NULL) {
        fprintf(stderr, "Memory allocation for vertices failed!\n");
        exit(EXIT_FAILURE);
    }

    glGenBuffers(1, &(shaderPtr->VBOId));
    glBindBuffer(GL_ARRAY_BUFFER, shaderPtr->VBOId);
    glBufferData(GL_ARRAY_BUFFER, shaderPtr->VBOlength * sizeof(GLfloat), vertices, GL_STATIC_DRAW);

    // Specify the layout of the vertex data
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(GLfloat), (void*)0);
    glEnableVertexAttribArray(0);

    free(vertices);

    GLenum ErrorCheckValue = glGetError();
    if (ErrorCheckValue != GL_NO_ERROR) {
        fprintf(stderr, "ERROR: Could not create a VBO\n");
        exit(EXIT_FAILURE);
    }
}

void destroyVBO(shader* shaderPtr) {
    GLenum errorCheckValue = glGetError();
    glDisableVertexAttribArray(1);
    glDisableVertexAttribArray(0);

    glBindBuffer(GL_ARRAY_BUFFER, 0);

    glDeleteBuffers(1, &(shaderPtr->VBOId));

    errorCheckValue = glGetError();
    if (errorCheckValue != GL_NO_ERROR) {
        fprintf(stderr, "ERROR: Could not destroy the VBO.\n");
        exit(EXIT_FAILURE);
    }
}

void createShaders(shader* shaderPtr, const char* fragSource) {

    GLenum errorCheckValue = glGetError();
    GLint compile_ok = GL_FALSE;

    shaderPtr->programId = glCreateProgram();

    shaderPtr->vertexShaderId = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(shaderPtr->vertexShaderId, 1, &vertexShaderSource, NULL);
    glCompileShader(shaderPtr->vertexShaderId);

    glGetShaderiv(shaderPtr->vertexShaderId, GL_COMPILE_STATUS, &compile_ok);
    if(!compile_ok) {
        GLchar infoLog[512];
        glGetShaderInfoLog(shaderPtr->vertexShaderId, 512, NULL, infoLog);
        fprintf(stderr, "Vertex shader compilation failed: %s\n", infoLog);
        exit(EXIT_FAILURE);
    }

    shaderPtr->fragmentShaderId = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(shaderPtr->fragmentShaderId,1, &fragSource, NULL);
    glCompileShader(shaderPtr->fragmentShaderId);

    glGetShaderiv(shaderPtr->fragmentShaderId, GL_COMPILE_STATUS, &compile_ok);
    if(!compile_ok) {
        GLchar infoLog[512];
        glGetShaderInfoLog(shaderPtr->fragmentShaderId, 512, NULL, infoLog);
        fprintf(stderr, "Fragment shader compilation failed: %s\n", infoLog);
        exit(EXIT_FAILURE);
    }

    errorCheckValue = glGetError();
    if (errorCheckValue != GL_NO_ERROR) {
         fprintf(stderr, "ERROR: Could not create the at the end of shaders %s.\n", glGetErrorStr(errorCheckValue));
    }
}

void destroyShaders(shader* shaderPtr) {
    GLenum errorCheckValue = glGetError();
    glUseProgram(0);

    glDetachShader(shaderPtr->programId, shaderPtr->vertexShaderId);
    glDetachShader(shaderPtr->programId, shaderPtr->fragmentShaderId);

    glDeleteShader(shaderPtr->fragmentShaderId);
    glDeleteShader(shaderPtr->vertexShaderId);

    glDeleteProgram(shaderPtr->programId);

    errorCheckValue = glGetError();
    if (errorCheckValue != GL_NO_ERROR) {
        fprintf(stderr, "ERROR: Could not destroy %s.\n", glGetErrorStr(errorCheckValue));
        exit(EXIT_FAILURE);
    }
}

// void updateShaderSource(char* sourceBuffer, const char* parameterName, float parameterValue) {
//   const char *location = strstr(sourceBuffer, parameterName);

//   if (location != NULL) {
//         int index = location - sourceBuffer;
//         char angle_buffer[PARAMETER_LENGTH+1];
//         snprintf(angle_buffer, PARAMETER_LENGTH+1, "%f", parameterValue);
//         for (int i = 0; i < PARAMETER_LENGTH; i++) {
//             uint offset = strlen(parameterName) + 3; //Plus 3 to make room for " = "
//             sourceBuffer[index+offset+i] = angle_buffer[i];
//         }
//   } else {
//         printf("Substring not found\n");
//         exit(EXIT_FAILURE);
//   }

  

// }

void updateShader(shader* shaderPtr, const char* uniformName, float uniformValue) {
    int location  = glGetUniformLocation(shaderPtr->programId, uniformName);
    if (location == -1) {
        printf("Uniform '%s' not found or optimized out.\n", uniformName);
        exit(EXIT_FAILURE);
    }
    glUniform1f(location, uniformValue);
}

shader buildShaders(float angle, float spatial, float cyclesPerSecond) {
    shader myShader;

    createShaders(&myShader, sinFragSource);
    //createShaders(gaborFragSource, &myShader);
    //createShaders(squareFragSource, &myShader);
    createVBO(&myShader);

    myShader.angle = angle;
    myShader.spatial = spatial;
    myShader.cyclesPerSecond = cyclesPerSecond;
    myShader.aspectRatio = (float)mode.hdisplay / mode.vdisplay;

    glAttachShader(myShader.programId, myShader.vertexShaderId);
    glAttachShader(myShader.programId, myShader.fragmentShaderId);
    glLinkProgram(myShader.programId);
    GLenum errorCheckValue = glGetError();
    if (errorCheckValue != GL_NO_ERROR) {
        fprintf(stderr, "ERROR: Could not attach shaders %s.\n", glGetErrorStr(errorCheckValue));
    }
    return myShader;
}

void loadShader(GLconfig* configPtr, shader* shaderPtr) {

    glUseProgram(shaderPtr->programId);

    long start_time = get_time_micros();
    updateShader(shaderPtr, "angle", shaderPtr->angle);
    updateShader(shaderPtr, "spatial", shaderPtr->spatial);
    updateShader(shaderPtr, "aspectRatio", shaderPtr->aspectRatio);
    updateShader(shaderPtr, "cyclesPerSecond", shaderPtr->cyclesPerSecond);
    printf("Updating shaders took %ld microseconds\n", get_time_micros()-start_time);

    GLenum errorCheckValue = glGetError();
    if (errorCheckValue != GL_NO_ERROR) {
        fprintf(stderr, "ERROR: Could not use shader program %s.\n", glGetErrorStr(errorCheckValue));
    }
    configPtr->currentShaderPtr = shaderPtr;
}

int getDeviceDisplay(GLconfig* config) {
    // You can try chaning this to "card0" if "card1" does not work.
    config->device = open("/dev/dri/card1", O_RDWR | O_CLOEXEC);
    if (getDisplay(&(config->display), config->device) != 0)  {
        fprintf(stderr, "Unable to get EGL display\n");
        close(config->device);
        return EXIT_FAILURE;
    }
    return 1;
} 

int EGLinit(GLconfig* configPtr) {
    int major, minor;
    if (eglInitialize(configPtr->display, &major, &minor) && eglBindAPI(EGL_OPENGL_API)) {
        printf("Initialized EGL version: %d.%d\n", major, minor);
        return 1;
    }
    fprintf(stderr, "Failed to get EGL version! Error: %s\n",  eglGetErrorStr());
    eglTerminate(configPtr->display);
    gbmClean(configPtr->device);
    return EXIT_FAILURE;
    
}

int EGLGetConfig(EGLConfig **configs, int *configIndex, GLconfig* configPtr) {
    EGLint count, numConfigs;
    eglGetConfigs(configPtr->display, NULL, 0, &count);
    *configs = malloc(count * sizeof(EGLConfig));
    if (!eglChooseConfig(configPtr->display, configAttribs, *configs, count, &numConfigs))  {
        fprintf(stderr, "Failed to get EGL configs! Error: %s\n", eglGetErrorStr());
        eglTerminate(configPtr->display);
        gbmClean(configPtr->device);
        return EXIT_FAILURE;
    }

    // I am not exactly sure why the EGL config must match the GBM format.
    // But it works!
    *configIndex = matchConfigToVisual(configPtr->display, GBM_FORMAT_XRGB8888, *configs, numConfigs);
    if (configIndex < 0)  {
        fprintf(stderr, "Failed to find matching EGL config! Error: %s\n",  eglGetErrorStr());
        eglTerminate(configPtr->display);
        gbm_surface_destroy(gbmSurface);
        gbm_device_destroy(gbmDevice);
        free(configs);
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

int EGLGetContext(EGLConfig config, GLconfig* configPtr) {
    
    configPtr->context = eglCreateContext(configPtr->display, config, EGL_NO_CONTEXT, contextAttribs);
    if (configPtr->context == EGL_NO_CONTEXT)  {
        fprintf(stderr, "Failed to create EGL context! Error: %s\n",  eglGetErrorStr());
        eglTerminate(configPtr->display);
        gbmClean(configPtr->device);
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

int EGLGetSurface(EGLConfig config, GLconfig* configPtr) {
    configPtr->surface = eglCreateWindowSurface(configPtr->display, config, gbmSurface, NULL);
    if (configPtr->surface == EGL_NO_SURFACE)  {
        fprintf(stderr, "Failed to create EGL surface! Error: %s\n",  eglGetErrorStr());
        eglDestroyContext(configPtr->display, configPtr->context);
        eglTerminate(configPtr->display);
        gbmClean(configPtr->device);
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;

}

void EGLcleanup(GLconfig* configPtr) {
    eglDestroyContext(configPtr->display, configPtr->context);
    eglDestroySurface(configPtr->display, configPtr->surface);
    eglTerminate(configPtr->display);
    gbmClean(configPtr->device);
}

int checkViewport(GLconfig* configPtr) {
    int desiredWidth = mode.hdisplay;
    int desiredHeight = mode.vdisplay;

    // Set GL Viewport size, always needed!
    glViewport(0, 0, desiredWidth, desiredHeight);

    // Get GL Viewport size and test if it is correct.
    GLint viewport[4];
    glGetIntegerv(GL_VIEWPORT, viewport);

    // viewport[2] and viewport[3] are viewport width and height respectively
    printf("GL Viewport size: %dx%d\n", viewport[2], viewport[3]);

    if (viewport[2] != desiredWidth || viewport[3] != desiredHeight)   {
        fprintf(stderr, "Error! The glViewport returned incorrect values! Something is wrong!\n");
        EGLcleanup(configPtr);
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

GLconfig setup(void) {
    
    GLconfig config;
    getDeviceDisplay(&config);
    EGLinit(&config);

    EGLConfig *EGLconfigs;
    int configIndex;
    EGLGetConfig(&EGLconfigs, &configIndex, &config);
    EGLGetContext(EGLconfigs[configIndex], &config);
    EGLGetSurface(EGLconfigs[configIndex], &config);
    free(EGLconfigs); //configs is malloced in EGLGetConfig()
    eglMakeCurrent(config.display, config.surface, config.surface, config.context);

    checkViewport(&config);
    return config;
}

void mainloop(GLconfig* configPtr) {

    // Clear whole screen (front buffer)
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    long start_time = get_time_micros();
    float elapsed_time;
    int timeLocation = glGetUniformLocation(configPtr->currentShaderPtr->programId, "time");
    for (int q = 0; q < 120; q++) {
        //printf("We got here\n");
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        elapsed_time = (float) (get_time_micros() - start_time)/1000000;
        glUniform1f(timeLocation, elapsed_time);
        glDrawArrays(GL_TRIANGLES, 0, configPtr->currentShaderPtr->VBOlength);
        gbmSwapBuffers(&(configPtr->display), &(configPtr->surface), configPtr->device);
    }
    printf("We did 120 frames in %f\n", (double)(get_time_micros() - start_time)/1000000);
}



static PyObject* py_setup(PyObject *self, PyObject *args) {

    GLconfig* configPtr = malloc(sizeof(GLconfig));
    *configPtr = setup();

    PyObject* config_capsule = PyCapsule_New(configPtr, "config", NULL);
    Py_INCREF(config_capsule);
    return config_capsule;
}

static PyObject* py_buildShader(PyObject *self, PyObject *args) {

    float angle;
    float spatial;
    float cyclesPerSecond;
    if (!PyArg_ParseTuple(args, "fff", &angle, &spatial, &cyclesPerSecond)) {
         return NULL;
    }

    shader* shaderPtr = malloc(sizeof(shader));
    *shaderPtr = buildShaders(angle, spatial, cyclesPerSecond);
    
    PyObject* shader_capsule = PyCapsule_New(shaderPtr, "shader", NULL);
    return shader_capsule;
}

static PyObject* py_loadShader(PyObject* self, PyObject* args) {
    PyObject* config_capsule;
    PyObject* shader_capsule;

    if (!PyArg_ParseTuple(args, "OO", &config_capsule, &shader_capsule)) {
        return NULL;
    }
    GLconfig* configPtr = PyCapsule_GetPointer(config_capsule, "config");
    shader* shaderPtr = PyCapsule_GetPointer(shader_capsule, "shader");
    loadShader(configPtr, shaderPtr);
    Py_RETURN_NONE;
}

static PyObject* py_display(PyObject* self, PyObject* args) {
    PyObject* config_capsule;
    if (!PyArg_ParseTuple(args, "O", &config_capsule)) {
        return NULL;
    }
    GLconfig* configPtr = PyCapsule_GetPointer(config_capsule,"config");
    mainloop(configPtr);
    Py_RETURN_NONE;
}

// Method definition table
static PyMethodDef methods[] = {
    {"setup", py_setup, METH_NOARGS, "Config EGL context"},
    {"build_shader", py_buildShader, METH_VARARGS, "Build Shaders"}, 
    {"load_shader", py_loadShader, METH_VARARGS, "Use Shader"},
    {"display", py_display, METH_VARARGS, "Display Something"},
    {NULL, NULL, 0, NULL}  // Sentinel
};

// Module definition structure
static struct PyModuleDef module = {
    PyModuleDef_HEAD_INIT,
    "rpg",   // Module name
    NULL,         // Module documentation
    -1,           // Size of per-interpreter state of the module
    methods
};
// Module initialization function
PyMODINIT_FUNC PyInit_rpg(void) {
    Py_Initialize();
    return PyModule_Create(&module);
}
